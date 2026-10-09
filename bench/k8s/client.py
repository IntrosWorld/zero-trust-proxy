#!/usr/bin/env python3
"""In-cluster probes and local decision-path load; never load-test a public origin."""
import argparse
from collections import Counter
import concurrent.futures
import json
from pathlib import Path
import socket
import ssl
import statistics
import time


def probe(host, token, destination='blocked.invalid', connect=False, delay=0, timeout=5):
    start=time.monotonic()
    try:
        with socket.create_connection((host,3128),timeout=timeout) as stream:
            authority=destination+':443'
            first=(f'CONNECT {authority} HTTP/1.1\r\n' if connect else f'GET http://{destination}/ HTTP/1.1\r\n')
            stream.sendall(first.encode())
            if delay:time.sleep(delay)
            stream.sendall(f'Host: {authority if connect else destination}\r\nProxy-Authorization: Bearer {token}\r\nConnection: close\r\n\r\n'.encode())
            data=b''
            while b'\r\n\r\n' not in data:
                part=stream.recv(4096)
                if not part:break
                data+=part
            status=int(data.split(b' ',2)[1]) if data.startswith(b'HTTP/1.1 ') else 0
            return status,time.monotonic()-start
    except (OSError,ValueError):return 0,time.monotonic()-start


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--host',default='zt-proxy.zt-system.svc.cluster.local')
    p.add_argument('--destination',default='blocked.invalid')
    p.add_argument('--connect',action='store_true')
    p.add_argument('--seconds',type=float,default=0)
    p.add_argument('--concurrency',type=int,default=1)
    p.add_argument('--delay',type=float,default=0)
    p.add_argument('--expected',type=int,default=403)
    p.add_argument('--token-file',default='/run/secrets/proxy-token')
    args=p.parse_args();token=Path(args.token_file).read_text().strip()
    deadline=time.monotonic()+args.seconds;start=time.monotonic()
    def worker(_):
        rows=[]
        while True:
            rows.append(probe(args.host,token,args.destination,args.connect,args.delay))
            if time.monotonic()>=deadline:break
        return rows
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        rows=[row for batch in pool.map(worker,range(args.concurrency)) for row in batch]
    counts=Counter(s for s,t in rows);timings=sorted(t*1000 for s,t in rows);elapsed=time.monotonic()-start
    print(json.dumps(dict(requests=len(rows),statuses=dict(counts),expected_status=args.expected,
                         failed_requests=sum(n for s,n in counts.items() if s!=args.expected),elapsed_seconds=elapsed,
                         requests_per_second=len(rows)/elapsed,p50_ms=statistics.median(timings),p99_ms=timings[min(len(timings)-1,int(len(timings)*.99))],
                         workload=('CONNECT handshake to origin' if args.connect else 'policy decisions for a denied destination; no origin traffic'))))

if __name__=='__main__':main()
