"""Use SentinelAgent's existing EgressClient and DLP with an external zt-proxy.

Add this directory and your SentinelAgent checkout to PYTHONPATH. No SentinelAgent
source changes or Python proxy listener are required. This adapter covers HTTP
clients; SentinelAgent's browser gateway has a separate lifecycle.
"""
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
import httpx
from sentinel_agent.egress.client import EgressBlocked, EgressClient, current_gateway
from sentinel_agent.egress.dlp import DLPScanner
from sentinel_agent.egress.telemetry import AuditLog


class ZTGateway:
    enabled = True

    def __init__(self, proxy_url, token, policy, *, scanner=None, audit_root=Path('artifacts/egress'), run_id='zt-proxy'):
        self.policy = policy
        self.scanner = scanner or DLPScanner()
        self.proxy = SimpleNamespace(url=httpx.Proxy(proxy_url, headers={'Proxy-Authorization': f'Bearer {token}'}))
        self.audit = AuditLog(run_id, audit_root)
        self.events = []

    async def check(self, url):
        return await self.policy.evaluate_url(url)

    def record(self, **fields):
        self.audit.write(**fields)
        self.events.append(fields)

    @contextmanager
    def activate(self):
        token = current_gateway.set(self)
        try:
            yield self
        finally:
            current_gateway.reset(token)


class ZTClient(EgressClient):
    """Existing DLP/policy pipeline with explicit TLS verification configuration."""

    def __init__(self, *, gateway, timeout=120, verify=True):
        super().__init__(gateway=gateway, timeout=timeout)
        self.verify = verify

    async def __aenter__(self):
        async def check_response(response):
            rule = response.headers.get('X-ZT-Block')
            # Only proxy error responses carry this header; never include body/URL.
            if response.status_code in (400, 403, 407, 408, 429, 431, 502, 503, 504) and rule:
                known = {'auth', 'rate-limit', 'port', 'ip-literal', 'suspicious-host',
                         'denylisted', 'not-allowlisted', 'private-ip', 'tls-required',
                         'request', 'request-body', 'dns', 'dns-timeout', 'connect',
                         'connect-timeout', 'timeout', 'header-timeout'}
                raise EgressBlocked(rule if rule in known else 'proxy')
        self.client = httpx.AsyncClient(timeout=self.timeout, trust_env=False,
                                       follow_redirects=False, proxy=self.gateway.proxy.url,
                                       verify=self.verify, event_hooks={'response': [check_response]})
        return self
