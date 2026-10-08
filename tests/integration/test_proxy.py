import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import time
import pytest
from support import Proxy, backend, request, tunnel, tls_context, receive_all, PAYLOAD, BINARY


def test_http_and_bodies(tmp_path):
    with backend(tmp_path) as port, Proxy(tmp_path, port) as p:
        assert p.request(request(port=port)).endswith(b'local backend ok\n')
        for extra, body in [('Content-Length: 4\r\n', b'data'), ('Transfer-Encoding: chunked\r\n', b'4\r\ndata\r\n0\r\n\r\n')]:
            response = p.request(request(port=port, method='POST', extra=extra, body=body))
            assert response.endswith(b'data'), response
        # Real curl exercises absolute-URI requests and proxy configuration.
        result = subprocess.run(['curl', '-fsS', '--noproxy', '', '-x', f'http://127.0.0.1:{p.port}', f'http://allowed.test:{port}/'], capture_output=True, check=True)
        assert result.stdout == b'local backend ok\n'


def test_tls_download(tmp_path):
    with backend(tmp_path, tls=True) as port, Proxy(tmp_path, port) as p:
        with tls_context().wrap_socket(tunnel(p, port), server_hostname='allowed.test') as s:
            s.sendall(b'GET /large HTTP/1.1\r\nHost: allowed.test\r\nConnection: close\r\n\r\n')
            response = receive_all(s).split(b'\r\n\r\n', 1)[1]
            assert hashlib.sha256(response).digest() == hashlib.sha256(PAYLOAD).digest()
        result = subprocess.run(['curl', '-kfsS', '--noproxy', '', '-x', f'http://127.0.0.1:{p.port}', f'https://allowed.test:{port}/'], capture_output=True, check=True)
        assert result.stdout == b'local backend ok\n'


@pytest.mark.parametrize('host,port,method,rule', [
    ('evil.test',443,'CONNECT','not-allowlisted'), ('private.test',443,'CONNECT','private-ip'),
    ('mixed.test',443,'CONNECT','private-ip'), ('169.254.169.254',443,'CONNECT','ip-literal'),
    ('[::1]',443,'CONNECT','ip-literal'), ('[::ffff:127.0.0.1]',443,'CONNECT','ip-literal'),
    ('[fd00::1]',443,'CONNECT','ip-literal'), ('xn--allowed-9za.test',443,'CONNECT','suspicious-host'),
    ('allowed.test',22,'CONNECT','port'), ('allowed.test',25,'CONNECT','port'),
    ('private.test',80,'GET','tls-required'),
])
def test_policy_blocks(tmp_path,host,port,method,rule):
    with Proxy(tmp_path, 12345) as p:
        assert f'X-ZT-Block: {rule}\r\n'.encode() in p.request(request(host,port,method))


def test_sni_and_not_tls(tmp_path):
    with backend(tmp_path, tls=True) as port, Proxy(tmp_path, port) as p:
        with tunnel(p,port) as s:
            s.sendall(b'SSH-2.0-evil\r\n')
            assert s.recv(1024) == b''
        with pytest.raises((ssl.SSLError, OSError)):
            tls_context().wrap_socket(tunnel(p,port), server_hostname='evil.test')
        time.sleep(.1)
        rules = [r['rule_id'] for r in p.records()]
        assert 'sni-mismatch' in rules and 'not-tls' in rules


def test_fragmented_hello(tmp_path):
    with backend(tmp_path,tls=True) as port, Proxy(tmp_path,port) as p:
        incoming,outgoing=ssl.MemoryBIO(),ssl.MemoryBIO()
        tls=tls_context().wrap_bio(incoming,outgoing,server_hostname='allowed.test')
        with tunnel(p,port) as s:
            with pytest.raises(ssl.SSLWantReadError):
                tls.do_handshake()
            hello=outgoing.read()
            for start in range(0,len(hello),7):
                s.sendall(hello[start:start+7])
                time.sleep(.001)
            while True:
                incoming.write(s.recv(16384))
                try:
                    tls.do_handshake()
                    s.sendall(outgoing.read())
                    break
                except ssl.SSLWantReadError:
                    if outgoing.pending:
                        s.sendall(outgoing.read())
            tls.write(b'GET / HTTP/1.1\r\nHost: allowed.test\r\nConnection: close\r\n\r\n')
            s.sendall(outgoing.read())
            result=b''
            while b'local backend ok\n' not in result:
                incoming.write(s.recv(16384))
                try:
                    result += tls.read(16384)
                except ssl.SSLWantReadError:
                    pass
            assert b'200 OK' in result


def test_auth_rate_limits(tmp_path):
    with Proxy(tmp_path,12345,require_auth=True,tokens=['correct'],rate=1,burst=2) as p:
        for token in ['', 'Proxy-Authorization: Bearer wrong\r\n']:
            assert b'407' in p.request(request(extra=token))
        good='Proxy-Authorization: Bearer correct\r\n'
        for _ in range(2):
            assert b'not-allowlisted' in p.request(request('evil.test',443,'CONNECT',good))
        assert b'429' in p.request(request('evil.test',443,'CONNECT',good))


def test_smuggling_limits_slowloris(tmp_path):
    with Proxy(tmp_path,12345,header_timeout_ms=200) as p:
        assert b'400' in p.request(request(extra='Content-Length: 1\r\nTransfer-Encoding: chunked\r\n'))
        assert b'431' in p.request(b'G'*9000)
        with p.connect() as s:
            s.sendall(b'G')
            time.sleep(.4)
            assert b'408' in receive_all(s)
        assert b'200' in p.admin_get('/healthz')


def test_connection_limits(tmp_path):
    with Proxy(tmp_path,12345,max_per_client=8,max_connections=10) as p:
        sockets=[]
        try:
            for _ in range(30):
                sockets.append(p.connect())
            time.sleep(.1)
            metrics=p.admin_get().decode()
            active=int(metrics.split('\nzt_active_connections ')[1].splitlines()[0])
            assert active<=8
            assert b'200' in p.admin_get('/healthz')
        finally:
            for s in sockets:
                s.close()
        time.sleep(.1)
        assert b'not-allowlisted' in p.request(request('evil.test',443,'CONNECT'))


def test_reload_and_logs(tmp_path):
    with backend(tmp_path) as port, Proxy(tmp_path,port,header_timeout_ms=5000) as p:
        def accepted():
            return int(p.admin_get().split(b'\nzt_connections_total ')[1].splitlines()[0])
        def wait_accepted(minimum):
            deadline=time.monotonic()+3
            while accepted()<minimum:
                assert time.monotonic()<deadline,'worker did not accept the connection'
                time.sleep(.01)
        wait_accepted(1) # The readiness connection must have been accepted first.
        before=accepted()
        # Wait for actual accept, not just the client-side TCP handshake. Valgrind
        # can delay accept beyond a fixed sleep; snapshots are taken at accept.
        with p.connect() as old:
            old.sendall(b'GET ')
            wait_accepted(before+1)
            p.reload(denylist=['allowed.test'])
            old.sendall(request(port=port)[4:])
            assert receive_all(old).endswith(b'local backend ok\n')
        assert b'denylisted' in p.request(request(port=port))
        p.path.write_text('{bad JSON')
        p.process.send_signal(__import__('signal').SIGHUP)
        time.sleep(.1)
        assert b'denylisted' in p.request(request(port=port))
        assert b'zt_decisions_total{rule="denylisted"}' in p.admin_get()
    records=p.records()
    assert any(r['host']=='allowed.test' and r['decision']=='allow' for r in records)
    assert 'reload rejected' in p.stderr_path.read_text()


def test_concurrency_and_wildcard(tmp_path):
    with backend(tmp_path) as port, Proxy(tmp_path,port,workers=4) as p:
        def fetch(_):
            return p.request(request('a.wild.test',port)).endswith(b'local backend ok\n')
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            assert all(pool.map(fetch,range(64)))


def test_config_safety(tmp_path):
    config=tmp_path/'bad.json'
    for data in [{'listen':'0.0.0.0'}, {'test_mode':True}, {'admin':'0.0.0.0'}, {'unknown_key':1}, {'port':0}, {'require_auth':True}]:
        config.write_text(json.dumps(data))
        result=subprocess.run([str(BINARY),'--config',str(config),'--check-config'],capture_output=True)
        assert result.returncode != 0


@pytest.mark.parametrize('ip',['127.0.0.1','192.168.1.1','10.0.0.5','169.254.169.254','100.64.0.1','::1','::ffff:127.0.0.1','fd00::1','fe80::1'])
def test_private_dns(tmp_path,ip):
    with Proxy(tmp_path,12345,test_dns={'private.test':[ip]}) as p:
        assert b'X-ZT-Block: private-ip' in p.request(request('private.test',443,'CONNECT'))


def test_dns_pinning(tmp_path):
    # Rebind between CONNECT and ClientHello. The existing connection retains its
    # checked sockaddr and config snapshot; new requests check the changed answer.
    with backend(tmp_path,tls=True) as port, Proxy(tmp_path,port) as p:
        raw=tunnel(p,port)
        p.reload(test_dns={'allowed.test':['10.0.0.5']},test_private_hosts=[])
        with tls_context().wrap_socket(raw,server_hostname='allowed.test') as s:
            s.sendall(b'GET / HTTP/1.1\r\nHost: allowed.test\r\nConnection: close\r\n\r\n')
            assert receive_all(s).endswith(b'local backend ok\n')
        assert b'private-ip' in p.request(request(port=port,method='CONNECT'))
    allowed=[r for r in p.records() if r['host']=='allowed.test' and r['decision']=='allow']
    assert allowed and allowed[0]['resolved_ip']=='127.0.0.1'


def test_thousands_of_connections(tmp_path):
    with Proxy(tmp_path,12345,max_per_client=16,max_connections=64,header_timeout_ms=3000) as p:
        sockets=[]
        try:
            for _ in range(2000):
                sockets.append(p.connect())
            with socket.socket() as other:
                other.settimeout(5)
                other.bind(('127.0.0.2',0))
                other.connect(('127.0.0.1',p.port))
                other.sendall(request('evil.test',443,'CONNECT'))
                assert b'not-allowlisted' in receive_all(other)
            assert b'200' in p.admin_get('/healthz')
        finally:
            for s in sockets:
                s.close()
    assert sum(r['rule_id']=='connection-limit' for r in p.records())>=1900


def test_ipv6_listener(tmp_path):
    # The product supports separate IPv4/IPv6 bind addresses, not dual-stack sockets.
    from support import free_port
    port,admin=free_port(),free_port()
    config=tmp_path/'v6.json'
    config.write_text(json.dumps(dict(listen='::1',port=port,admin_port=admin,workers=1,log_path=str(tmp_path/'v6.jsonl'))))
    p=subprocess.Popen([str(BINARY),'--config',str(config)],stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            try:
                s=socket.create_connection(('::1',port),timeout=.1)
                break
            except ConnectionRefusedError:
                time.sleep(.02)
        else:
            raise AssertionError('IPv6 listener did not start')
        with s:
            s.sendall(request('evil.test',443,'CONNECT'))
            assert b'not-allowlisted' in receive_all(s)
    finally:
        p.terminate()
        _,err=p.communicate(timeout=10)
        assert p.returncode==0,err


def test_half_close_and_backpressure(tmp_path):
    import threading
    listener=socket.socket()
    listener.bind(('127.0.0.1',0))
    listener.listen()
    port=listener.getsockname()[1]
    payload=bytes(range(256))*32768
    errors=[]
    def serve():
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(10)
                incoming=receive_all(peer)
                assert incoming.startswith(b'\x16\x03')
                peer.sendall(payload)
                peer.shutdown(socket.SHUT_WR)
        except Exception as e:
            errors.append(e)
    thread=threading.Thread(target=serve)
    thread.start()
    try:
        with Proxy(tmp_path,port,idle_timeout_ms=10000) as p:
            with tunnel(p,port) as s:
                incoming,outgoing=ssl.MemoryBIO(),ssl.MemoryBIO()
                tls=tls_context().wrap_bio(incoming,outgoing,server_hostname='allowed.test')
                with pytest.raises(ssl.SSLWantReadError):
                    tls.do_handshake()
                s.sendall(outgoing.read())
                s.shutdown(socket.SHUT_WR)
                time.sleep(.15) # Fill kernel send buffers before draining.
                result=receive_all(s)
                assert hashlib.sha256(result).digest()==hashlib.sha256(payload).digest()
    finally:
        listener.close()
        thread.join(timeout=10)
    assert not thread.is_alive() and not errors,errors


def test_shutdown_drains(tmp_path):
    with backend(tmp_path,tls=True) as port, Proxy(tmp_path,port,drain_timeout_ms=3000) as p:
        with tls_context().wrap_socket(tunnel(p,port),server_hostname='allowed.test') as s:
            p.process.terminate()
            time.sleep(.1)
            s.sendall(b'GET / HTTP/1.1\r\nHost: allowed.test\r\nConnection: close\r\n\r\n')
            assert receive_all(s).endswith(b'local backend ok\n')


def test_large_streaming_upload(tmp_path):
    data=bytes(range(256))*16384
    with backend(tmp_path) as port, Proxy(tmp_path,port) as p:
        with p.connect() as s:
            s.sendall(request(port=port,method='POST',extra=f'Content-Length: {len(data)}\r\n'))
            for i in range(0,len(data),4096):
                s.sendall(data[i:i+4096])
            result=receive_all(s).split(b'\r\n\r\n',1)[1]
            assert hashlib.sha256(result).digest()==hashlib.sha256(data).digest()


def test_header_bomb_and_bad_chunks(tmp_path):
    with backend(tmp_path) as port, Proxy(tmp_path,port) as p:
        assert b'431' in p.request(request(port=port,extra='X: a\r\n'*101))
        assert b'431' in p.request(request(port=port,extra='X: '+('a'*16384)+'\r\n'))
        assert b'400' in p.request(request(port=port,method='POST',extra='Transfer-Encoding: chunked\r\n',body=b'Z\r\n'))
        assert b'400' in p.request(request(port=port,extra='Connection: transfer-encoding\r\n'))


def test_system_dns_and_address_fallback(tmp_path):
    with backend(tmp_path) as port, Proxy(tmp_path,port,allowlist=['localhost'],http_allowlist=['localhost'],test_dns={},test_private_hosts=['localhost']) as p:
        assert p.request(request('localhost',port)).endswith(b'local backend ok\n')
    assert any(r['resolved_ip']=='127.0.0.1' and r['decision']=='allow' for r in p.records())


def test_tls_without_sni(tmp_path):
    with backend(tmp_path,tls=True) as port, Proxy(tmp_path,port) as p:
        with tls_context().wrap_socket(tunnel(p,port),server_hostname=None) as s:
            s.sendall(b'GET / HTTP/1.1\r\nHost: allowed.test\r\nConnection: close\r\n\r\n')
            assert receive_all(s).endswith(b'local backend ok\n')


def test_nonloopback_auth(tmp_path):
    with Proxy(tmp_path,12345,listen='0.0.0.0',require_auth=True,tokens=['correct'],test_mode=False,test_dns={},test_private_hosts=[]) as p:
        assert b'407' in p.request(request('evil.test',443,'CONNECT'))
        assert b'not-allowlisted' in p.request(request('evil.test',443,'CONNECT',extra='Proxy-Authorization: Bearer correct\r\n'))


def test_config_json_validation(tmp_path):
    path=tmp_path/'json.json'
    invalid=['{"rate": 1e}', '{"port": 3128, "port": 3129}', '{"log_path": "\\ud800"}', '{"log_path": "bad\\u0000path"}', '{"allowlist":["bad..host"]}', '{"tokens":["bad\\u0000token"]}', '{"workers": null}', '{"workers": 1.5}']
    for data in invalid:
        path.write_text(data)
        result=subprocess.run([str(BINARY),'--config',str(path),'--check-config'],capture_output=True)
        assert result.returncode!=0,data
    path.write_text('{"rate":1e2,"log_path":"\\u03b1\\ud83d\\ude80.jsonl","allowlist":["EXAMPLE.COM"]}')
    result=subprocess.run([str(BINARY),'--config',str(path),'--check-config'],capture_output=True)
    assert result.returncode==0,result.stderr
