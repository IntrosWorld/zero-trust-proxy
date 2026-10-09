#!/usr/bin/env python3
"""Versioned policy service. C++ is the single source of policy validation rules."""
import hmac
import json
import os
from pathlib import Path
import sqlite3
import signal
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs


def decode(data):
    def pairs(items):
        obj = {}
        for key, value in items:
            if key in obj:
                raise ValueError('duplicate JSON key')
            obj[key] = value
        return obj
    def constant(_):
        raise ValueError('nonfinite number')
    return json.loads(data, object_pairs_hook=pairs, parse_constant=constant)


class Store:
    def __init__(self, path, binary, bootstrap):
        self.binary = binary
        self.condition = threading.Condition(threading.RLock())
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.executescript('''
            CREATE TABLE IF NOT EXISTS versions(version INTEGER PRIMARY KEY, document TEXT NOT NULL, created REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS audit(id INTEGER PRIMARY KEY, actor TEXT, time REAL, old_version INTEGER, new_version INTEGER, diff TEXT);
        ''')
        self.heartbeats = {}
        self.rejected = 0
        self.started = time.time()
        if not self.db.execute('SELECT 1 FROM versions LIMIT 1').fetchone():
            self.replace(bootstrap, 'bootstrap')

    def validate(self, policy, version):
        if not isinstance(policy, dict):
            raise ValueError('policy must be an object')
        policy = dict(policy, policy_version=version)
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json') as file:
            json.dump(policy, file, allow_nan=False)
            file.flush()
            result = subprocess.run([self.binary, '--validate-policy', file.name],
                                    capture_output=True, text=True, timeout=5)
        if result.returncode:
            raise ValueError(result.stderr.strip())
        return json.loads(result.stdout)

    def current(self):
        row = self.db.execute('SELECT document, created FROM versions ORDER BY version DESC LIMIT 1').fetchone()
        return (json.loads(row[0]), row[1]) if row else (None, 0)

    def get(self, etag='', wait=0):
        with self.condition:
            deadline = time.monotonic() + min(30, max(0, wait))
            while True:
                document, _ = self.current()
                tag = '"' + document['sha256'] + '"'
                remaining = deadline - time.monotonic()
                if tag != etag or remaining <= 0:
                    return (304 if etag == tag else 200), document, tag
                self.condition.wait(remaining)

    def replace(self, policy, actor):
        with self.condition:
            old, _ = self.current()
            version = old['version'] + 1 if old else 1
            try:
                document = self.validate(policy, version)
            except (ValueError, subprocess.SubprocessError):
                self.rejected += 1
                raise
            now = time.time()
            diff = [k for k, v in document['policy'].items()
                    if k != 'policy_version' and (not old or old['policy'].get(k) != v)]
            with self.db:
                self.db.execute('INSERT INTO versions VALUES(?,?,?)', (version, json.dumps(document), now))
                self.db.execute('INSERT INTO audit(actor,time,old_version,new_version,diff) VALUES(?,?,?,?,?)',
                                (actor, now, old['version'] if old else 0, version, json.dumps(diff)))
            self.condition.notify_all()
            return document

    def rollback(self, version):
        with self.condition:
            row = self.db.execute('SELECT document FROM versions WHERE version=?', (version,)).fetchone()
            if not row:
                raise ValueError('unknown historical version')
            return self.replace(json.loads(row[0])['policy'], 'admin:rollback')

    def heartbeat(self, data):
        if not isinstance(data, dict) or set(data) != {'pod', 'policy_version', 'ready'}:
            raise ValueError('expected pod, policy_version and ready')
        if not isinstance(data['pod'], str) or not 1 <= len(data['pod']) <= 253:
            raise ValueError('invalid pod')
        if type(data['policy_version']) is not int or not 0 <= data['policy_version'] <= 2147483647 or type(data['ready']) is not bool:
            raise ValueError('invalid heartbeat')
        with self.condition:
            now = time.time()
            old = self.heartbeats.get(data['pod'], {})
            applied = old.get('applied_at', now) if old.get('policy_version') == data['policy_version'] else now
            self.heartbeats[data['pod']] = dict(data, last_seen=now, applied_at=applied)
            # Bound memory even when names churn indefinitely.
            self.heartbeats = {k: v for k, v in self.heartbeats.items() if now-v['last_seen'] < 300}
            if len(self.heartbeats) > 4096:
                self.heartbeats.pop(min(self.heartbeats, key=lambda k: self.heartbeats[k]['last_seen']))

    def rollout(self):
        with self.condition:
            document, created = self.current()
            now = time.time()
            pods = [dict(v, stale=now-v['last_seen'] > 45) for v in self.heartbeats.values()]
            live = [p for p in pods if not p['stale']]
            complete = bool(live) and all(p['ready'] and p['policy_version'] == document['version'] for p in live)
            return dict(version=document['version'], pods=pods, complete=complete,
                        elapsed_seconds=max(0, (max(p['applied_at'] for p in live) if complete else now)-created))


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass  # Never log Authorization headers or token-bearing requests.

    def send(self, status, value, etag=None, content_type='application/json'):
        body = (json.dumps(value).encode() if content_type == 'application/json' else value.encode()) if status != 304 else b''
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        if etag:
            self.send_header('ETag', etag)
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def dispatch(self):
        self.connection.settimeout(35)
        route = urlsplit(self.path)
        query = parse_qs(route.query)
        store = self.server.store
        if self.command == 'GET' and route.path == '/healthz':
            return self.send(200, {'status': 'ok'})
        if self.command == 'GET' and route.path == '/metrics':
            with store.condition:
                current, _ = store.current()
                return self.send(200, f'zt_control_policy_version {current["version"]}\nzt_control_policy_rejected_total {store.rejected}\nzt_control_heartbeats {len(store.heartbeats)}\n', content_type='text/plain; version=0.0.4')
        admin = self.command == 'PUT' or route.path in ('/v1/policy/rollback', '/v1/audit')
        supplied = self.headers.get('Authorization', '')
        expected = self.server.admin_token if admin else self.server.read_token
        if not hmac.compare_digest(supplied, 'Bearer '+expected):
            return self.send(401, {'error': 'unauthorized'})
        body = None
        if self.command in ('PUT', 'POST'):
            if self.headers.get('Transfer-Encoding'):
                raise ValueError('transfer encoding unsupported')
            lengths = self.headers.get_all('Content-Length') or ['0']
            if len(lengths) != 1:
                raise ValueError('ambiguous body length')
            length = int(lengths[0])
            if not 0 <= length <= 1024*1024:
                raise ValueError('body limit')
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError('truncated body')
            body = decode(raw) if raw else {}
        if self.command == 'GET' and route.path == '/v1/policy':
            status, doc, tag = store.get(self.headers.get('If-None-Match', ''), float(query.get('wait', ['0'])[0]))
            return self.send(status, doc, tag)
        if self.command == 'PUT' and route.path == '/v1/policy':
            return self.send(200, store.replace(body, 'admin'))
        if self.command == 'POST' and route.path == '/v1/policy/rollback':
            return self.send(200, store.rollback(int(query.get('to', ['0'])[0])))
        if self.command == 'POST' and route.path == '/v1/heartbeat':
            store.heartbeat(body)
            return self.send(200, {'ok': True})
        if self.command == 'GET' and route.path == '/v1/rollout':
            return self.send(200, store.rollout())
        if self.command == 'GET' and route.path == '/v1/audit':
            with store.condition:
                return self.send(200, store.db.execute('SELECT actor,time,old_version,new_version,diff FROM audit ORDER BY id DESC LIMIT 100').fetchall())
        return self.send(404, {'error': 'not found'})

    def handle_method(self):
        try:
            self.dispatch()
        except (ValueError, KeyError, TypeError, subprocess.SubprocessError) as e:
            self.send(400, {'error': str(e)})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True

    do_GET = do_PUT = do_POST = handle_method


class Server(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address, store, admin_token, read_token):
        super().__init__(address, Handler)
        self.store, self.admin_token, self.read_token = store, admin_token, read_token
        self.slots = threading.BoundedSemaphore(128)
    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise
    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


if __name__ == '__main__':
    bootstrap = decode(Path(os.environ.get('ZT_BOOTSTRAP_POLICY', '/etc/zt-policy/policy.json')).read_text())
    store = Store(os.environ.get('ZT_POLICY_DB', '/data/policy.db'), os.environ.get('ZT_BINARY', '/usr/local/bin/zt-proxy'), bootstrap)
    admin_token = Path(os.environ['ZT_POLICY_ADMIN_TOKEN_FILE']).read_text().strip()
    read_token = Path(os.environ['ZT_POLICY_READ_TOKEN_FILE']).read_text().strip()
    if len(admin_token) < 32 or len(read_token) < 32 or admin_token == read_token:
        raise ValueError('distinct admin and read tokens of at least 32 characters required')
    server = Server((os.environ.get('ZT_POLICY_BIND', '127.0.0.1'), int(os.environ.get('ZT_POLICY_PORT', '8080'))), store, admin_token, read_token)
    def stop(*_):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        with store.condition:
            store.db.close()
