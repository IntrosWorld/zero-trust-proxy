#!/usr/bin/env python3
"""Render saved benchmark data without rerunning or inventing measurements."""
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent/'results'
rows=json.loads((root/'http.json').read_text())
fig,axes=plt.subplots(1,2,figsize=(11,4),constrained_layout=True)
for concurrency in sorted({r['connections'] for r in rows}):
    proxy=sorted((r for r in rows if r['connections']==concurrency and r['mode']=='proxy'),key=lambda r:r['workers'])
    workers=[r['workers'] for r in proxy]
    axes[0].plot(workers,[r['requests_per_second'] for r in proxy],marker='o',label=f'{concurrency} connections')
    axes[1].plot(workers,[r['requests_per_second']/r['workers'] for r in proxy],marker='o',label=f'{concurrency} connections')
for ax,title in zip(axes,['Total request rate','Request rate per worker']):
    ax.set(title=title,xlabel='Worker threads',ylabel='Requests / second')
    ax.set_xticks(sorted({r['workers'] for r in rows if r['mode']=='proxy'}))
    ax.grid(alpha=.25)
    ax.legend()
fig.suptitle('Local HTTP/1.1, 30-second wrk runs, connection close')
fig.savefig(root/'scaling.svg')
fig.savefig(root/'scaling.png',dpi=160)
