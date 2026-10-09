import importlib.util
import json
from pathlib import Path
import socket
import socketserver
import threading
import time
import pytest
from support import Proxy, backend, request, BINARY

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('zt_control',ROOT/'control-plane/server.py')
control=importlib.util.module_from_spec(spec);spec.loader.exec_module(control)


def test_policy_sync_fail_static_and_rejection(tmp_path):
    secret=tmp_path/'sync-token';secret.write_text('reader')
    store=control.Store(str(tmp_path/'db'),str(BINARY),{'allowlist':['allowed.test'],'http_allowlist':['allowed.test']})
    server=control.Server(('127.0.0.1',0),store,'admin','reader')
    thread=threading.Thread(target=server.serve_forever);thread.start()
    try:
        with backend(tmp_path) as port:
            good=dict(allowlist=['allowed.test'],http_allowlist=['allowed.test'],allowed_ports=[port],rate=1000,burst=1000)
            doc=store.replace(good,'test')
            with Proxy(tmp_path/'proxy',port,policy_url=f'http://127.0.0.1:{server.server_port}',policy_token_file=str(secret),policy_poll_ms=50,policy_wait_seconds=1) as p:
                def version():return json.loads(p.admin_get('/policy').split(b'\r\n\r\n',1)[1])['version']
                def wait_version(v):
                    until=time.monotonic()+4
                    while version()!=v and time.monotonic()<until:time.sleep(.02)
                    assert version()==v
                wait_version(doc['version'])
                assert p.request(request(port=port)).endswith(b'local backend ok\n')
                doc=store.replace(dict(good,denylist=['allowed.test']),'test')
                wait_version(doc['version'])
                assert p.request(request(port=port)).startswith(b'HTTP/1.1 403')
                # A corrupted persisted response simulates a compromised/misbehaving control plane.
                with store.condition:
                    invalid=dict(doc);invalid['policy']=dict(doc['policy'],workers=2)
                    store.db.execute('UPDATE versions SET document=? WHERE version=?',(json.dumps(invalid),doc['version']))
                    store.db.commit()
                # Force a fresh 200/ETag so the proxy validates the bad document.
                with store.condition:
                    invalid['sha256']='0'*64
                    store.db.execute('UPDATE versions SET document=? WHERE version=?',(json.dumps(invalid),doc['version']));store.db.commit();store.condition.notify_all()
                until=time.monotonic()+4
                while b'zt_policy_reload_total{result="rejected"} 0' in p.admin_get() and time.monotonic()<until:time.sleep(.05)
                assert b'zt_policy_reload_total{result="rejected"} 0' not in p.admin_get()
                assert version()==doc['version']
                server.shutdown();server.server_close();thread.join()
                time.sleep(.25)
                assert p.request(request(port=port)).startswith(b'HTTP/1.1 403')
                assert b'zt_policy_sync_errors_total 0' not in p.admin_get()
    finally:
        if thread.is_alive():server.shutdown();server.server_close();thread.join()
        store.db.close()


class RedisHandler(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            start=self.rfile.readline()
            if not start:return
            count=int(start[1:]);args=[]
            for _ in range(count):
                size=int(self.rfile.readline()[1:]);args.append(self.rfile.read(size).decode());assert self.rfile.read(2)==b'\r\n'
            if args[0]=='SCRIPT':response=b'$40\r\n'+b'a'*40+b'\r\n'
            elif args[0]=='EVALSHA':
                with self.server.lock:
                    key=args[3];rate=float(args[4]);burst=float(args[5]);now=time.monotonic()
                    tokens,last=self.server.buckets.get(key,(burst,now));tokens=min(burst,tokens+(now-last)*rate)
                    allowed=tokens>=1;tokens-=int(allowed);self.server.buckets[key]=(tokens,now)
                response=b':1\r\n' if allowed else b':0\r\n'
            else:response=b'+OK\r\n'
            # Deliberately fragmented RESP delivery.
            for part in (response[:2],response[2:]):self.wfile.write(part);self.wfile.flush()

class FakeRedis(socketserver.ThreadingTCPServer):
    allow_reuse_address=True;daemon_threads=True
    def __init__(self):
        super().__init__(('127.0.0.1',0),RedisHandler);self.lock=threading.Lock();self.buckets={}


def test_redis_shared_identity_and_timeout(tmp_path):
    redis=FakeRedis();thread=threading.Thread(target=redis.serve_forever);thread.start()
    try:
        with backend(tmp_path) as port:
            settings=dict(redis_host='127.0.0.1',redis_port=redis.server_address[1],rate=.01,burst=3,require_auth=True,tokens=['test'])
            with Proxy(tmp_path/'a',port,**settings) as a,Proxy(tmp_path/'b',port,**settings) as b:
                results=[p.request(request(port=port,extra='Proxy-Authorization: Bearer test\r\n')).split(b' ')[1] for p in (a,b,a,b)]
                assert results==[b'200',b'200',b'200',b'429']
                assert b'zt_ratelimit_backend{backend="redis"} 1' in a.admin_get()
    finally:redis.shutdown();redis.server_close();thread.join()

@pytest.mark.parametrize('closed',[False,True])
def test_redis_outage_mode(tmp_path,closed):
    with socket.socket() as s:s.bind(('127.0.0.1',0));unused=s.getsockname()[1]
    with backend(tmp_path) as port,Proxy(tmp_path/'proxy',port,redis_host='127.0.0.1',redis_port=unused,redis_timeout_ms=50,redis_fail_closed=closed) as p:
        assert p.request(request(port=port)).startswith(b'HTTP/1.1 '+(b'503' if closed else b'200'))
        assert b'zt_ratelimit_backend{backend="local"} 1' in p.admin_get()
        assert b'zt_redis_errors_total 0' not in p.admin_get()
