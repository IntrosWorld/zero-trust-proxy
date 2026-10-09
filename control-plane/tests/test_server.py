import importlib.util
import json
import os
from pathlib import Path
import threading
import time
import urllib.request
import urllib.error
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('control', ROOT/'control-plane/server.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)

@pytest.fixture
def store(tmp_path):
    result = control.Store(str(tmp_path/'policy.db'), str(ROOT/os.environ.get('ZT_BINARY', 'build/release/zt-proxy')), {'allowlist':['example.com']})
    yield result
    result.db.close()

def test_versions_validation_rollback_persistence(store):
    assert store.get()[1]['version'] == 1
    original = store.get()[1]['policy']
    for bad in ({'workers':2}, {'rate':-1}, {'allowed_ports':[0]}, {'allowlist':['bad host']}):
        with pytest.raises(ValueError): store.replace(bad,'test')
    assert store.get()[1]['version'] == 1
    changed = store.replace({'denylist':['example.com']},'test')
    assert changed['version'] == 2
    rolled = store.rollback(1)
    assert rolled['version'] == 3
    assert rolled['policy']['allowlist'] == original['allowlist']
    assert store.db.execute('SELECT count(*) FROM audit').fetchone()[0] == 3
    assert store.rejected == 4

def test_etag_long_poll(store):
    _, _, tag = store.get()
    start = time.monotonic()
    assert store.get(tag, .1)[0] == 304
    assert time.monotonic()-start >= .09
    timer = threading.Timer(.1, lambda: store.replace({'rate':5},'test'))
    timer.start()
    assert store.get(tag, 1)[0] == 200
    timer.join()

def test_heartbeat(store):
    store.heartbeat({'pod':'p1','policy_version':1,'ready':True})
    assert store.rollout()['complete']
    store.replace({'rate':5},'test')
    assert not store.rollout()['complete']
    with pytest.raises(ValueError): store.heartbeat({'pod':'p1','policy_version':True,'ready':True})

def test_http_auth_and_etag(store):
    server = control.Server(('127.0.0.1',0), store, 'admin', 'reader')
    thread = threading.Thread(target=server.serve_forever);thread.start()
    url = f'http://127.0.0.1:{server.server_port}/v1/policy'
    try:
        with pytest.raises(urllib.error.HTTPError) as e: urllib.request.urlopen(url)
        assert e.value.code == 401
        request = urllib.request.Request(url, headers={'Authorization':'Bearer reader'})
        with urllib.request.urlopen(request) as r:
            tag=r.headers['ETag']; assert json.load(r)['version']==1
        request.add_header('If-None-Match',tag)
        with pytest.raises(urllib.error.HTTPError) as e: urllib.request.urlopen(request)
        assert e.value.code == 304
        request=urllib.request.Request(url,data=b'{"rate": 4}',method='PUT',headers={'Authorization':'Bearer reader'})
        with pytest.raises(urllib.error.HTTPError) as e: urllib.request.urlopen(request)
        assert e.value.code==401
        request=urllib.request.Request(url,data=b'{"rate": 4}',method='PUT',headers={'Authorization':'Bearer admin'})
        with urllib.request.urlopen(request) as r: assert json.load(r)['version']==2
    finally:
        server.shutdown();server.server_close();thread.join()

def test_duplicate_keys():
    with pytest.raises(ValueError): control.decode('{"rate":1,"rate":2}')
