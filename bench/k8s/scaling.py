#!/usr/bin/env python3
"""E3/E4 and Redis overhead. All high-rate traffic receives intentional local 403 decisions."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
from run import k, pods, ready, wait, forward, Control, baseline, await_policy, deployment, OUT, ROOT


def scale(n):
    k('-n','zt-system','scale','deployment/zt-proxy',f'--replicas={n}')
    wait(lambda:len(pods())==n and all(ready(p) for p in pods()),120)


def wrk(seconds=10,concurrency=64):
    raw=k('-n','apps','exec','deployment/zt-client','--','wrk','-t2',f'-c{concurrency}',f'-d{seconds}s','--latency','-s','/opt/wrk.lua','http://zt-proxy.zt-system:3128/')
    def match(pattern):
        m=re.search(pattern,raw);return m.group(1) if m else None
    return dict(requests_per_second=float(match(r'Requests/sec:\s+([0-9.]+)')),
                p50=match(r'50%\s+(\S+)'),p99=match(r'99%\s+(\S+)'),raw=raw,
                workload='authenticated HTTP requests to blocked.invalid; 403 is the expected successful policy decision, not origin throughput')


def scaling(control,args):
    k('-n','zt-system','delete','hpa','zt-proxy','--ignore-not-found')
    await_policy(control.put(rate=1e9,burst=1e9))
    results=[]
    try:
        for n in [1,2,3,6]:
            scale(n);await_policy(control.call())
            result=wrk(args.seconds);result['replicas']=n;results.append(result)
            print(json.dumps(result),flush=True)
    finally:scale(3)
    result=dict(experiment='E3',passed=True,results=results,note='one physical host; benchmark client is limited to two CPU cores; scaling is not linear')
    (OUT/'E3.json').write_text(json.dumps(result,indent=2)+'\n')
    latency={}
    try:
        for shared in [False,True]:
            deployment(redis_host='redis' if shared else '')
            await_policy(control.call());latency['redis' if shared else 'local']=wrk(args.seconds)
    finally:deployment(redis_host='redis')
    (OUT/'redis-latency.json').write_text(json.dumps(latency,indent=2)+'\n')


def hpa(control,args):
    scale(3);await_policy(control.put(rate=1e9,burst=1e9))
    subprocess.run(['helm','upgrade','zt-proxy',str(ROOT/'deploy/helm/zt-proxy'),'-n','zt-system','--reuse-values','--set','hpa.enabled=true','--wait','--timeout','3m'],check=True,stdout=subprocess.DEVNULL)
    await_policy(control.call())
    traffic=subprocess.Popen(['kubectl','-n','apps','exec','deployment/zt-client','--','wrk','-t2','-c128',f'-d{args.load_seconds}s','-s','/opt/wrk.lua','http://zt-proxy.zt-system:3128/'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    start=time.monotonic();timeline=[]
    end=start+args.load_seconds+args.cooldown_seconds
    while time.monotonic()<end:
        status=json.loads(k('-n','zt-system','get','hpa','zt-proxy','-o','json'))['status']
        row=dict(seconds=round(time.monotonic()-start,2),load_running=traffic.poll() is None,
                 current=status.get('currentReplicas'),desired=status.get('desiredReplicas'),metrics=status.get('currentMetrics',[]),conditions=status.get('conditions',[]))
        timeline.append(row);print(json.dumps(row),flush=True);time.sleep(5)
    stdout,stderr=traffic.communicate(timeout=10)
    rose=any((r['current'] or 0)>3 for r in timeline)
    fell=rose and timeline[-1]['current']==3
    result=dict(experiment='E4',passed=rose and fell,timeline=timeline,scaled_up=rose,scaled_down=fell,wrk=stdout,stderr=stderr)
    (OUT/'E4.json').write_text(json.dumps(result,indent=2)+'\n')
    if not result['passed']:raise RuntimeError('HPA did not complete both transitions within observation window')


def main():
    p=argparse.ArgumentParser();p.add_argument('experiment',choices=['E3','E4']);p.add_argument('--seconds',type=int,default=15);p.add_argument('--load-seconds',type=int,default=90);p.add_argument('--cooldown-seconds',type=int,default=150);args=p.parse_args()
    with forward('zt-policy',8080) as port:
        (scaling if args.experiment=='E3' else hpa)(Control(port),args)
if __name__=='__main__':main()
