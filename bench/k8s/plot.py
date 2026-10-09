#!/usr/bin/env python3
"""Plot recorded local E3/E4 measurements; requires matplotlib."""
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

out = Path(__file__).resolve().parents[1] / 'results/k8s'
scaling = json.loads((out / 'E3.json').read_text())['results']
timeline = json.loads((out / 'E4.json').read_text())['timeline']
fig, axes = plt.subplots(1, 2, figsize=(12, 4.5), constrained_layout=True)
axes[0].plot([r['replicas'] for r in scaling],
             [r['requests_per_second'] for r in scaling], 'o-', color='#2563eb')
axes[0].set(xlabel='Proxy replicas', ylabel='Policy decisions / second',
            title='E3: one shared host, 15 seconds per count', xticks=[1, 2, 3, 6])
axes[0].set_ylim(bottom=0)
axes[1].step([r['seconds'] for r in timeline], [r['current'] for r in timeline],
             where='post', label='Observed replicas', color='#2563eb')
axes[1].step([r['seconds'] for r in timeline], [r['desired'] for r in timeline],
             where='post', label='Desired replicas', color='#ea580c', linestyle='--')
axes[1].set(xlabel='Seconds since load started', ylabel='Replicas', yticks=[3, 4, 5, 6],
            title='E4: CPU autoscaling and cooldown')
load_end = next(r['seconds'] for r in timeline if not r['load_running'])
axes[1].axvspan(0, load_end, color='#94a3b8', alpha=.15, label='Load observed running')
axes[1].legend(loc='lower center', fontsize=8)
for ax in axes:
    ax.grid(alpha=.2)
fig.suptitle('Authenticated HTTP 403 decisions; not origin forwarding throughput', fontsize=11)
fig.savefig(out / 'scaling.png', dpi=160)
plt.close(fig)
