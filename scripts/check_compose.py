#!/usr/bin/env python3
"""Verify the real Compose deployment in an isolated project; preserve other services."""
import json
from pathlib import Path
import socket
import subprocess
import time
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    # Host networking shares ports. Fail before creating anything if occupied.
    listeners = []
    try:
        for port in (3128, 9100):
            listener = socket.socket()
            listeners.append(listener)
            listener.bind(('127.0.0.1', port))
    finally:
        for listener in listeners:
            listener.close()

    project = f'zt-proxy-check-{uuid.uuid4().hex[:12]}'
    base = ['docker', 'compose', '-p', project, '-f', 'deploy/compose.yml']
    report = {'project': project, 'passed': False, 'timestamp': time.time()}
    out = ROOT / 'bench/results'
    out.mkdir(parents=True, exist_ok=True)

    def compose(*args):
        return subprocess.run(base + list(args), cwd=ROOT, text=True,
                              capture_output=True, check=True).stdout

    def inspect(container):
        return json.loads(subprocess.check_output(
            ['docker', 'inspect', container], text=True))[0]

    try:
        compose('config', '--quiet')
        compose('up', '-d', '--no-build', '--wait', '--wait-timeout', '45')
        container = compose('ps', '-q', 'proxy').strip()
        state = inspect(container)
        host = state['HostConfig']
        assert state['State']['Health']['Status'] == 'healthy'
        assert state['Config']['User'] == '10001:10001'
        assert host['ReadonlyRootfs']
        assert 'ALL' in host['CapDrop']
        assert any(x.startswith('no-new-privileges') for x in host['SecurityOpt'])
        assert host['Memory'] == 512 * 1024 * 1024
        assert host['PidsLimit'] == 128
        assert host['NetworkMode'] == 'host'
        with urllib.request.urlopen('http://127.0.0.1:9100/healthz', timeout=3) as response:
            assert response.status == 200
        with socket.create_connection(('127.0.0.1', 3128), timeout=3) as client:
            client.sendall(b'CONNECT evil.test:443 HTTP/1.1\r\nHost: evil.test:443\r\n\r\n')
            assert client.recv(4096).startswith(b'HTTP/1.1 403')
        compose('exec', '-T', 'proxy', 'sh', '-c',
                'printf persistent > /var/lib/zt-proxy/compose-check')
        compose('kill', '--signal', 'SIGHUP', 'proxy')
        deadline = time.monotonic() + 5
        while 'configuration reloaded' not in compose('logs', '--no-color', 'proxy'):
            if time.monotonic() >= deadline:
                raise RuntimeError('SIGHUP reload not observed')
            time.sleep(0.1)
        compose('stop', 'proxy')
        stopped = inspect(container)['State']
        assert stopped['ExitCode'] == 0 and not stopped['OOMKilled'], stopped
        compose('down')
        compose('up', '-d', '--no-build', '--wait', '--wait-timeout', '45')
        assert compose('exec', '-T', 'proxy', 'cat',
                       '/var/lib/zt-proxy/compose-check') == 'persistent'
        access = compose('exec', '-T', 'proxy', 'cat', '/var/lib/zt-proxy/access.jsonl')
        assert any(json.loads(line).get('host') == 'evil.test' for line in access.splitlines())
        report.update(passed=True, health='healthy', runtime_user=state['Config']['User'],
                      read_only=host['ReadonlyRootfs'], cap_drop=host['CapDrop'],
                      security_options=host['SecurityOpt'], memory_bytes=host['Memory'],
                      pids_limit=host['PidsLimit'], network=host['NetworkMode'],
                      policy_denial=True, reload=True, graceful_exit_code=stopped['ExitCode'],
                      volume_persistence=True)
        print('Compose passed: health, policy, reload, graceful stop, persistent logs and runtime restrictions.')
    finally:
        # This random project owns its containers and volume; no global pruning.
        compose('down', '--volumes', '--remove-orphans')
        (out / 'docker-compose.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
