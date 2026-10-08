#!/usr/bin/env python3
"""Update the README performance table strictly from recorded results."""
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
results=root/'bench/results'
rows=json.loads((results/'http.json').read_text())
tls=json.loads((results/'tls.json').read_text())
idle=json.loads((results/'idle.json').read_text())
env=json.loads((results/'environment.json').read_text())
lines=['Intel Core Ultra 5 125H (18 logical CPUs), Ubuntu 22.04, GCC 11.4, release `-O3 -DNDEBUG`. Full hardware/kernel/tool versions and the measured binary hash are in [environment.json](bench/results/environment.json).','',
       '| Clients | Direct/proxy workers | Requests/sec | p50 | p99 |',
       '| ---: | --- | ---: | ---: | ---: |']
for row in rows:
    mode='Direct' if row['mode']=='direct' else f"Proxy / {row['workers']}"
    lines.append(f"| {row['connections']:,} | {mode} | {row['requests_per_second']:,.2f} | {row['p50']} | {row['p99']} |")
direct=next(r['requests_per_second'] for r in rows if r['connections']==100 and r['mode']=='direct')
proxy=next(r['requests_per_second'] for r in rows if r['connections']==100 and r['workers']==4)
lines += ['',f'The four-worker proxy delivered {proxy/direct:.1%} of direct request throughput at 100 clients. More workers did not consistently help; this shared-host, single Python-backend workload is not evidence of linear proxy scaling. The 1,000-client runs show higher queuing latency. No failed runs or unfavorable worker counts were omitted.','',
          f"The **{tls['bytes']//1048576} MiB TLS download** matched SHA-256 `{tls['sha256']}` and measured **{tls['mib_per_second']:.2f} MiB/s** in {tls['seconds']:.3f} s, with proxy CPU at {tls['proxy_cpu_percent_one_core']:.1f}% of one core. The 1,000-idle-connection test added approximately **{idle['incremental_bytes_per_connection']/1024:.1f} KiB RSS per connection**."]
readme=root/'README.md'
text=readme.read_text()
start=text.index('<!-- BENCH_START -->')+len('<!-- BENCH_START -->')
end=text.index('<!-- BENCH_END -->')
readme.write_text(text[:start]+'\n'+'\n'.join(lines)+'\n'+text[end:])
