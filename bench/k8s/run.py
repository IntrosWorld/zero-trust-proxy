#!/usr/bin/env python3
"""Repeatable local-cluster experiments. Raw results include failures, never invented numbers."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import socket
import statistics
import subprocess
import sys
import time
import urllib.request
import urllib.error

ROOT=Path(__file__).resolve().parents[2]
os.environ['PATH']=str(ROOT/'artifacts/bin')+':'+os.environ['PATH']
os.environ.setdefault('KUBECONFIG',str(ROOT/'artifacts/kubeconfig'))
OUT=ROOT/'bench/results/k8s';OUT.mkdir(parents=True,exist_ok=True)


def k(*args,**kw):
    return subprocess.run(['kubectl',*args],check=True,text=True,capture_output=True,**kw).stdout

def pods():
    return [p for p in json.loads(k('-n','zt-system','get','pods','-l','app=zt-proxy','-o','json'))['items']
            if not p['metadata'].get('deletionTimestamp') and p['status'].get('podIP')]

def ready(p):return any(c['type']=='Ready' and c['status']=='True' for c in p['status'].get('conditions',[]))

def wait(test,seconds=60):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        result=test()
        if result:return result
        time.sleep(.2)
    raise TimeoutError(f'condition failed within {seconds}s')

def admin_exec(code,*args):
    return k('-n','zt-system','exec','deployment/zt-admin','--','python3','-c',code,*args)

def observe():
    targets=[{'pod':p['metadata']['name'],'ip':p['status']['podIP']} for p in pods() if ready(p)]
    code='''import concurrent.futures,json,sys,urllib.request
items=json.loads(sys.argv[1])
def one(p):
    with urllib.request.urlopen('http://'+p['ip']+':9100/policy',timeout=3) as r:p['policy']=json.load(r)
    with urllib.request.urlopen('http://'+p['ip']+':9100/metrics',timeout=3) as r:p['metrics']=r.read().decode()
    return p
with concurrent.futures.ThreadPoolExecutor() as pool:print(json.dumps(list(pool.map(one,items))))
'''
    return json.loads(admin_exec(code,json.dumps(targets)))

def metric(row,name):
    m=re.search(r'^'+re.escape(name)+r' ([0-9.e+\-]+)$',row['metrics'],re.M)
    return float(m.group(1)) if m else 0

@contextmanager
def forward(service,remote,namespace='zt-system'):
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    p=subprocess.Popen(['kubectl','-n',namespace,'port-forward','service/'+service,f'{port}:{remote}'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def up():
        if p.poll() is not None:raise RuntimeError('port-forward exited')
        try:
            with socket.create_connection(('127.0.0.1',port),.2):return True
        except OSError:return False
    try:wait(up,15);yield port
    finally:p.terminate();p.wait(timeout=5)

class Control:
    def __init__(self,port):self.url=f'http://127.0.0.1:{port}'
    def call(self,path='/v1/policy',method='GET',data=None):
        token=(ROOT/'artifacts/secrets'/('policy-admin-token' if method!='GET' else 'policy-read-token')).read_text().strip()
        request=urllib.request.Request(self.url+path,data=json.dumps(data).encode() if data is not None else None,method=method,headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
        with urllib.request.urlopen(request,timeout=10) as r:return json.load(r)
    def put(self,**changes):
        policy=self.call()['policy'];policy.update(changes)
        return self.call(method='PUT',data=policy)

def await_policy(document,seconds=15):
    def applied():
        rows=observe()
        return rows if len(rows)==len(pods()) and len(rows)>0 and all(r['policy']['version']==document['version'] and r['policy']['sha256']==document['sha256'] for r in rows) else None
    return wait(applied,seconds)

def deployment(**changes):
    config=json.loads(k('-n','zt-system','get','configmap','zt-proxy-config','-o','json'))['data']['config.json']
    config=json.loads(config);config.update(changes)
    k('-n','zt-system','patch','configmap','zt-proxy-config','--type=merge','-p',json.dumps({'data':{'config.json':json.dumps(config)}}))
    k('-n','zt-system','rollout','restart','deployment/zt-proxy')
    k('-n','zt-system','rollout','status','deployment/zt-proxy','--timeout=120s')

def load(seconds=12,concurrency=24,delay=0):
    return subprocess.Popen(['kubectl','-n','apps','exec','deployment/zt-client','--','python3','/opt/client.py','--seconds',str(seconds),'--concurrency',str(concurrency),'--delay',str(delay)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)

def load_result(p):
    stdout,stderr=p.communicate(timeout=180)
    if p.returncode:raise RuntimeError(stderr)
    return json.loads(stdout)

def cluster_probes(destination,expected):
    targets=[p['status']['podIP'] for p in pods() if ready(p)]
    code='''import sys,json,pathlib
sys.path.insert(0,'/opt');from client import probe
token=pathlib.Path('/run/secrets/proxy-token').read_text().strip()
print(json.dumps([{'ip':ip,'status':probe(ip,token,sys.argv[2],True)[0]} for ip in json.loads(sys.argv[1])]))'''
    rows=json.loads(admin_exec(code,json.dumps(targets),destination))
    assert rows and all(r['status']==expected for r in rows),rows
    return rows

def baseline(control):
    return control.put(allowlist=['example.com','*.example.com'],denylist=[],http_allowlist=[],allowed_ports=[80,443],rate=100000,burst=100000)

def E1(control,args):
    await_policy(baseline(control))
    before={p['metadata']['name'] for p in pods()};old=sorted(before)[0]
    traffic=load();time.sleep(2);start=time.monotonic()
    k('-n','zt-system','delete','pod',old,'--wait=false')
    try:
        wait(lambda:any(ready(p) and p['metadata']['name'] not in before for p in pods()),90)
        recovery=time.monotonic()-start
    finally:result=load_result(traffic)
    return dict(load=result,replacement_ready_seconds=recovery,passed=result['failed_requests']==0,method='graceful Kubernetes pod deletion')

def E2(control,args):
    results={}
    for long_poll in [0,30]:
        deployment(policy_wait_seconds=long_poll,policy_poll_ms=200)
        await_policy(baseline(control));times=[]
        for _ in range(args.repeats):
            # First allow, then time the block rollout; only the block is timed.
            await_policy(control.put(denylist=[]))
            start=time.monotonic();doc=control.put(denylist=['example.com'])
            await_policy(doc);probes=cluster_probes('example.com',403)
            rollout=control.call('/v1/rollout')
            live={p['metadata']['name'] for p in pods()}
            assert live.issubset({p['pod'] for p in rollout['pods'] if p['policy_version']==doc['version']})
            times.append((time.monotonic()-start)*1000)
        results['long_poll' if long_poll else 'polling']={'samples_ms':times,'p50_ms':statistics.median(times),'max_ms':max(times),'replicas':len(probes),'poll_interval_ms':200,'includes':'API write + kubectl exec observations + every-pod deny probes + rollout read'}
    await_policy(baseline(control))
    return dict(passed=True,**results)

def E5(control,args):
    await_policy(baseline(control));results={}
    try:
        for graceful in [False,True]:
            deployment(shutdown_delay_ms=3000 if graceful else 0,drain_timeout_ms=10000 if graceful else 50)
            # Strategic patch preserves the container's image/resources while changing probes.
            patch={'spec':{'template':{'spec':{'containers':[{'name':'proxy','readinessProbe':{'httpGet':{'path':'/readyz','port':9100},'periodSeconds':1,'failureThreshold':1} if graceful else None}]}}}}
            k('-n','zt-system','patch','deployment','zt-proxy','--type=strategic','-p',json.dumps(patch))
            k('-n','zt-system','rollout','status','deployment/zt-proxy','--timeout=120s')
            await_policy(control.call())
            traffic=load(seconds=16,concurrency=48,delay=.2);time.sleep(2)
            image='zt-proxy:kubernetes-next' if graceful else 'zt-proxy:kubernetes'
            # Force a template change even if the desired tag is already set.
            patch={'spec':{'template':{'metadata':{'annotations':{'experiment/image-update':str(time.time())}},'spec':{'containers':[{'name':'proxy','image':image}]}}}}
            k('-n','zt-system','patch','deployment','zt-proxy','--type=strategic','-p',json.dumps(patch))
            k('-n','zt-system','rollout','status','deployment/zt-proxy','--timeout=120s')
            results['graceful' if graceful else 'baseline']=load_result(traffic)
    finally:deployment(shutdown_delay_ms=3000,drain_timeout_ms=10000)
    return dict(passed=results['graceful']['failed_requests']==0,improved=results['graceful']['failed_requests']<results['baseline']['failed_requests'],**results)

def E6(control,args):
    settings=json.loads(json.loads(k('-n','zt-system','get','configmap','zt-proxy-config','-o','json'))['data']['config.json'])
    samples={}
    try:
        for shared in [False,True]:
            deployment(redis_host='redis' if shared else '')
            await_policy(control.put(rate=2,burst=5))
            # Drain any buckets retained from a previous run (includes the initial generous policy burst).
            code='''import sys,json,time,pathlib,concurrent.futures
sys.path.insert(0,'/opt');from client import probe
hosts=json.loads(sys.argv[1]);token=pathlib.Path('/run/secrets/proxy-token').read_text().strip()
rows=[];start=time.monotonic()
for i in range(90):
    rows.append(probe(hosts[i%len(hosts)],token)[0]);time.sleep(.025)
print(json.dumps({'statuses':{str(s):rows.count(s) for s in set(rows)},'elapsed_seconds':time.monotonic()-start,'requests':len(rows)}))'''
            data=json.loads(admin_exec(code,json.dumps([p['status']['podIP'] for p in pods() if ready(p)])))
            data['accepted']=data['statuses'].get('403',0);data['allowed_bound']=5+2*data['elapsed_seconds']
            samples['redis' if shared else 'local']=data
        assert samples['redis']['accepted']<=samples['redis']['allowed_bound']+1,samples
        assert samples['local']['accepted']>samples['redis']['accepted'],samples
    finally:
        deployment(redis_host=settings['redis_host']);await_policy(baseline(control))
    return dict(passed=True,identity='shared authenticated bearer identity in Redis; local fallback keyed by source IP per worker',**samples)

def E7(control,args):
    await_policy(baseline(control));before=observe();k('-n','zt-system','scale','deployment/redis','--replicas=0')
    try:
        time.sleep(2);fallback=load_result(load(seconds=2,concurrency=4));rows=observe()
        assert all(metric(r,'zt_ratelimit_backend{backend="local"}')==1 for r in rows),rows
        assert sum(metric(r,'zt_redis_errors_total') for r in rows)>sum(metric(r,'zt_redis_errors_total') for r in before)
    finally:
        start=time.monotonic();k('-n','zt-system','scale','deployment/redis','--replicas=1')
        k('-n','zt-system','rollout','status','deployment/redis','--timeout=90s')
    def recovered():
        load_result(load(seconds=.5,concurrency=4))
        return all(metric(r,'zt_ratelimit_backend{backend="redis"}')==1 for r in observe())
    wait(recovered,20)
    return dict(passed=fallback['failed_requests']==0,fallback=fallback,recovery_seconds=time.monotonic()-start)

def E8(control,args):
    await_policy(baseline(control))
    direct=subprocess.run(['kubectl','-n','apps','exec','deployment/zt-client','--','curl','--noproxy','*','--max-time','4','-fsS','https://example.com'],capture_output=True,text=True)
    code='''import pathlib,subprocess,json
r=subprocess.run(['curl','--noproxy','','--proxy','http://zt-proxy.zt-system:3128','--proxy-header','Proxy-Authorization: Bearer '+pathlib.Path('/run/secrets/proxy-token').read_text().strip(),'--max-time','12','-fsS','https://example.com'],capture_output=True)
print(json.dumps({'exit_code':r.returncode,'bytes':len(r.stdout),'stderr':r.stderr.decode()}))'''
    via=json.loads(k('-n','apps','exec','deployment/zt-client','--','python3','-c',code))
    assert direct.returncode!=0 and via['exit_code']==0 and via['bytes']>0,(direct.stderr,via)
    return dict(passed=True,direct_exit_code=direct.returncode,proxy=via,cni='Calico v3.33.0',negative_case='same apps pod direct HTTPS denied; authenticated proxied HTTPS succeeds')

def E9(control,args):
    document=baseline(control);await_policy(document);before=observe()
    k('-n','zt-system','scale','deployment/zt-policy','--replicas=0')
    try:
        wait(lambda:not json.loads(k('-n','zt-system','get','pods','-l','app=zt-policy','-o','json'))['items'],60)
        time.sleep(4);traffic=load_result(load(seconds=2,concurrency=4));after=observe()
        assert all(r['policy']['sha256']==document['sha256'] for r in after)
        # DNS/control connect timeout can precede the next retry.
        wait(lambda:sum(metric(r,'zt_policy_sync_errors_total') for r in observe())>sum(metric(r,'zt_policy_sync_errors_total') for r in before),15)
    finally:
        k('-n','zt-system','scale','deployment/zt-policy','--replicas=1')
        k('-n','zt-system','rollout','status','deployment/zt-policy','--timeout=90s')
    return dict(passed=traffic['failed_requests']==0,load=traffic,retained_version=document['version'],retained_sha256=document['sha256'])

def E10(control,args):
    original=control.call();await_policy(original);before=observe()
    try:control.call(method='PUT',data={'rate':-1});raise AssertionError('invalid policy accepted')
    except urllib.error.HTTPError as e:assert e.code==400
    assert control.call()['sha256']==original['sha256']
    # Fault injection: a schema-valid but conflicting same-version response, modeling a stale/corrupt control store.
    code='''import sqlite3,json,subprocess,tempfile
c=sqlite3.connect('/data/policy.db');row=c.execute('SELECT version,document FROM versions ORDER BY version DESC LIMIT 1').fetchone();doc=json.loads(row[1]);doc['policy']['denylist']=['conflicting.invalid']
with tempfile.NamedTemporaryFile(mode='w') as f:
 json.dump(doc['policy'],f);f.flush();new=subprocess.check_output(['/usr/local/bin/zt-proxy','--validate-policy',f.name],text=True)
c.execute('UPDATE versions SET document=? WHERE version=?',(new,row[0]));c.commit();print('injected valid conflicting version')'''
    k('-n','zt-system','exec','deployment/zt-policy','--','python3','-c',code)
    try:
        wait(lambda:all(metric(r,'zt_policy_reload_total{result="rejected"}')>next(metric(b,'zt_policy_reload_total{result="rejected"}') for b in before if b['pod']==r['pod']) for r in observe()),40)
        assert all(r['policy']['sha256']==original['sha256'] for r in observe())
    finally:
        restore='import sqlite3,sys; c=sqlite3.connect("/data/policy.db");c.execute("UPDATE versions SET document=? WHERE version=?",(sys.argv[1],int(sys.argv[2])));c.commit()'
        k('-n','zt-system','exec','deployment/zt-policy','--','python3','-c',restore,json.dumps(original),str(original['version']))
    return dict(passed=True,invalid_put_status=400,proxy_rejected='valid schema, conflicting same version (explicit database fault injection)',retained_sha256=original['sha256'])


def main():
    parser=argparse.ArgumentParser();parser.add_argument('experiments',nargs='*',default=['E1','E2','E5','E6','E7','E8','E9','E10']);parser.add_argument('--repeats',type=int,default=20)
    args=parser.parse_args()
    failures=[]
    for name in args.experiments:
        start=time.time();result={'experiment':name,'started':start,'passed':False}
        print('Running '+name,flush=True)
        try:
            with forward('zt-policy',8080) as port:result.update(globals()[name](Control(port),args))
        except Exception as error:result['error']=str(error);failures.append(name)
        result['duration_seconds']=time.time()-start
        (OUT/(name+'.json')).write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result),flush=True)
        if not result['passed'] and name not in failures:failures.append(name)
    return bool(failures)

if __name__=='__main__':sys.exit(main())
