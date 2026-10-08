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
    with backend(tmp_path) as port, Proxy(tmp_path,port) as p:
        # Accepted connections retain their immutable configuration snapshot.
        with p.connect() as old:
            old.sendall(b'GET ')
            time.sleep(.05)
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
