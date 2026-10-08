#!/usr/bin/env python3
"""Run the real SentinelAgent EgressClient against a local HTTPS backend."""
import argparse
import asyncio
import json
from pathlib import Path
import ssl
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/integration'))
from support import Proxy,backend


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--sentinel-root',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'bench/results/sentinel.json')
    args=parser.parse_args()
    sys.path.insert(0,str(args.sentinel_root.resolve()))
    from sentinel_adapter import ZTGateway,ZTClient
    from sentinel_agent.egress.client import EgressBlocked,EgressError
    from sentinel_agent.egress.config import EgressConfig
    from sentinel_agent.egress.policy import DestinationPolicy
    async def run(port,proxy,directory):
        # Test fixture DNS used by the agent's independent policy check. The C++
        # proxy resolves and pins its own explicitly authorized test endpoint.
        async def fixture_dns(host,port):
            return ('8.8.8.8',)
        policy=DestinationPolicy(EgressConfig(provider_hosts=frozenset({'allowed.test'}),remote_ports=frozenset({port})),resolver=fixture_dns)
        gateway=ZTGateway(f'http://127.0.0.1:{proxy.port}','local-demo-token',policy,audit_root=Path(directory)/'audit')
        context=ssl.create_default_context(cafile=str(Path(directory)/'cert.pem'))
        url=f'https://allowed.test:{port}/'
        async with ZTClient(gateway=gateway,verify=context) as client:
            response=await client.get(url)
            assert response.text=='local backend ok\n'
            try:
                await client.post(url,json={'password':'synthetic-test-value'})
            except EgressBlocked as error:
                assert error.rule=='dlp'
            else:
                raise AssertionError('DLP did not block before encryption')
            try:
                await client.get(f'https://evil.test:{port}/')
            except EgressBlocked:
                pass
            else:
                raise AssertionError('Agent policy did not block unknown host')
            proxy.reload(denylist=['allowed.test'])
            try:
                await client.get(url)
            except EgressError:
                pass
            else:
                raise AssertionError('Proxy deny did not reach the agent')
        return gateway.events
    with tempfile.TemporaryDirectory(prefix='zt-sentinel-') as directory:
        with backend(directory,tls=True) as port,Proxy(directory,port,require_auth=True,tokens=['local-demo-token']) as proxy:
            events=asyncio.run(run(port,proxy,directory))
        records=[r for r in proxy.records() if r['host']]
        assert any(r['sni']=='allowed.test' and r['rule_id']=='allow' for r in records)
        assert any(r['rule_id']=='denylisted' for r in records)
        report=dict(agent_checkout=str(args.sentinel_root),http_client='actual SentinelAgent EgressClient through ZTClient adapter',tls_verified=True,dlp_blocked=True,agent_policy_blocked=True,proxy_block_surfaced=True,proxy_records=records,agent_events=events)
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,indent=2)+'\n')
        print('SentinelAgent: verified TLS request, DLP block, destination block and proxy rejection passed')
if __name__=='__main__':
    main()
