#!/usr/bin/env python3
"""Run all spec attack/legitimate categories and retain honest raw evidence."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

CASES = {
 'R1': 'test_policy_blocks[evil.test',
 'R2': 'test_private_dns',
 'R3': 'test_policy_blocks[169.',
 'R4': 'test_policy_blocks[[',
 'R5': 'test_dns_pinning',
 'R6': 'test_sni_and_not_tls',
 'R7': 'test_sni_and_not_tls',
 'R8': 'test_policy_blocks[xn--',
 'R9': 'test_policy_blocks[allowed.test',
 'R10': 'test_policy_blocks[private.test-80',
 'R11': 'test_auth_rate_limits',
 'R12': 'test_auth_rate_limits',
 'R13': 'test_smuggling_limits_slowloris',
 'R14': 'test_header_bomb_and_bad_chunks',
 'R15': 'test_smuggling_limits_slowloris',
 'R16': 'test_thousands_of_connections',
 'L1': 'test_tls_download',
 'L2': 'test_concurrency_and_wildcard',
 'L3': 'test_tls_download',
 'L4': 'test_fragmented_hello',
 'L5': 'test_concurrency_and_wildcard',
}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',default='bench/results/redteam.json')
    args=parser.parse_args()
    output=Path(args.output)
    output.parent.mkdir(parents=True,exist_ok=True)
    xml=output.with_suffix('.xml')
    result=subprocess.run([sys.executable,'-m','pytest','tests/integration','-q',f'--junitxml={xml}'],text=True,capture_output=True)
    print(result.stdout)
    output.with_suffix('.txt').write_text(result.stdout+result.stderr)
    import xml.etree.ElementTree as ET
    cases=list(ET.parse(xml).getroot().iter('testcase'))
    report={}
    for name,needle in CASES.items():
        matched=[c for c in cases if needle in c.attrib['name']]
        report[name]=dict(passed=bool(matched) and all(not list(c) for c in matched),tests=[c.attrib['name'] for c in matched])
    attacks=sum(report[f'R{i}']['passed'] for i in range(1,17))
    allowed=sum(report[f'L{i}']['passed'] for i in range(1,6))
    summary=dict(timestamp=time.time(),categories=report,blocked=attacks,attacks=16,block_rate=attacks/16,false_positive_rate=(5-allowed)/5,
                 note='Category coverage from local fixtures. L3 smoke uses 1 MiB; bench/run.py separately measures 500 MiB. R5 rebinding is modeled with a test-only local pinned endpoint and an atomic DNS mapping change.')
    output.write_text(json.dumps(summary,indent=2)+'\n')
    print(f'Attack categories: {attacks}/16; block rate {attacks/16:.1%}; false-positive categories: {5-allowed}/5')
    return result.returncode or (0 if attacks==16 and allowed==5 else 1)

if __name__=='__main__':
    raise SystemExit(main())
