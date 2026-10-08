"""Local-only proxy fixtures shared by integration tests and benchmarks."""
import contextlib
import http.server
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import ssl
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('ZT_BINARY', ROOT / 'build/debug/zt-proxy')).resolve()
PAYLOAD = bytes(range(256)) * 4096


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        data = PAYLOAD if self.path == '/large' else b'local backend ok\n'
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        if self.headers.get('Transfer-Encoding') == 'chunked':
            data = b''
            while True:
                n = int(self.rfile.readline().strip().split(b';')[0], 16)
                if not n:
                    while self.rfile.readline() != b'\r\n':
                        pass
                    break
                data += self.rfile.read(n)
                assert self.rfile.read(2) == b'\r\n'
        else:
            data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(data)


@contextlib.contextmanager
def backend(directory, tls=False):
    class Server(http.server.ThreadingHTTPServer):
        request_queue_size = 256
    server = Server(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    if tls:
        cert, key = Path(directory) / 'cert.pem', Path(directory) / 'key.pem'
        if not cert.exists():
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key), '-out', str(cert), '-days', '1', '-subj', '/CN=allowed.test', '-addext', 'subjectAltName=DNS:allowed.test'], check=True, capture_output=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_port
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class Proxy:
    def __init__(self, directory, backend_port, **overrides):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.port, self.admin = free_port(), free_port()
        self.path = self.directory / 'config.json'
        self.log = self.directory / 'access.jsonl'
        self.config = dict(listen='127.0.0.1', port=self.port, admin_port=self.admin, workers=1, resolver_threads=2,
                           allowlist=['allowed.test', '*.wild.test', 'private.test', 'mixed.test'],
                           http_allowlist=['allowed.test', '*.wild.test'], allowed_ports=[80, 443, backend_port],
                           test_mode=True, test_dns={'allowed.test': ['127.0.0.1'], 'a.wild.test': ['127.0.0.1'],
                           'private.test': ['10.0.0.5'], 'mixed.test': ['8.8.8.8', '127.0.0.1']},
                           test_private_hosts=['allowed.test', 'a.wild.test'], rate=10000, burst=10000,
                           header_timeout_ms=1000, idle_timeout_ms=3000, connect_timeout_ms=1000, dns_timeout_ms=1000,
                           drain_timeout_ms=500, log_path=str(self.log))
        self.config.update(overrides)
        self.write()
        self.stderr_path = self.directory / 'stderr.txt'
        self.stderr = self.stderr_path.open('w')
        self.process = subprocess.Popen(shlex.split(os.environ.get('ZT_RUNNER', '')) + [str(BINARY), '--config', str(self.path), '--allow-test-config'], stdout=subprocess.DEVNULL, stderr=self.stderr)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.stderr.close()
                raise RuntimeError(self.stderr_path.read_text())
            try:
                # Readiness on proxy listener ensures every component has started.
                with socket.create_connection(('127.0.0.1', self.port), timeout=.1):
                    break
            except OSError:
                time.sleep(.02)
        else:
            self.close()
            raise RuntimeError('proxy startup timed out')

    def write(self):
        temporary = self.path.with_suffix('.tmp')
        temporary.write_text(json.dumps(self.config))
        temporary.replace(self.path)

    def reload(self, **updates):
        before = self.stderr_path.read_text().count('configuration reloaded')
        self.config.update(updates)
        self.write()
        self.process.send_signal(signal.SIGHUP)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.stderr_path.read_text().count('configuration reloaded') > before:
                return
            time.sleep(.01)
        raise AssertionError(self.stderr_path.read_text())

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=5)

    def request(self, data):
        with self.connect() as s:
            s.sendall(data)
            return receive_all(s)

    def admin_get(self, path='/metrics'):
        with socket.create_connection(('127.0.0.1', self.admin), timeout=5) as s:
            s.sendall(f'GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
            return receive_all(s)

    def records(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('proxy failed to stop')
        self.stderr.close()
        errors = self.stderr_path.read_text()
        assert self.process.returncode == 0, errors
        assert 'ERROR: AddressSanitizer' not in errors and 'runtime error:' not in errors and 'WARNING: ThreadSanitizer' not in errors, errors

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def receive_all(s):
    data = bytearray()
    while True:
        chunk = s.recv(65536)
        if not chunk:
            return bytes(data)
        data.extend(chunk)


def request(host='allowed.test', port=80, method='GET', extra='', body=b'', path='/'):
    target = f'{host}:{port}' if method == 'CONNECT' else f'http://{host}:{port}{path}'
    return f'{method} {target} HTTP/1.1\r\nHost: {host}:{port}\r\n{extra}\r\n'.encode() + body


def tunnel(proxy, port, host='allowed.test'):
    s = proxy.connect()
    s.sendall(request(host, port, 'CONNECT'))
    response = b''
    while b'\r\n\r\n' not in response:
        chunk = s.recv(1)
        if not chunk:
            s.close()
            raise AssertionError(response)
        response += chunk
    assert b'200 Connection Established' in response, response
    return s


def tls_context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx
