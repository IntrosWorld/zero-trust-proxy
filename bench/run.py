#!/usr/bin/env python3
"""Reproducible wrk scaling, TLS throughput/hash and idle-memory measurements."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tests/integration'))
from support import Proxy,free_port,tunnel,tls_context,BINARY
ROOT=Path(__file__).resolve().parents[1]

@contextlib.contextmanager
def backend(directory,tls=False):
    port=free_port()
    cmd=[sys.executable,str(ROOT/'bench/backend.py'),'--port',str(port)]
    if tls:
        cert,key=Path(directory)/'cert.pem',Path(directory)/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=allowed.test'],check=True,capture_output=True)
        cmd+=['--cert',str(cert),'--key',str(key)]
    process=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1):
                    break
            except OSError:
                time.sleep(.02)
        else:
            raise RuntimeError('backend not ready')
        yield port
    finally:
        process.terminate()
        process.wait(timeout=5)

def run_wrk(url,concurrency,seconds,script=None,proxy=None):
    cmd=['wrk','-t4',f'-c{concurrency}',f'-d{seconds}s','--latency','--timeout','10s']
    if script:
        cmd+=['-s',str(script)]
    process=subprocess.Popen(cmd+[url],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    time.sleep(min(1,seconds/2))
    metrics=proxy.admin_get().decode() if proxy else ''
    sockets=subprocess.run(['ss','-tanp'],capture_output=True,text=True).stdout if proxy else ''
    text=process.communicate(timeout=seconds+20)[0]
    if process.returncode:
        raise RuntimeError(text)
    rate=float(re.search(r'Requests/sec:\s+([\d.]+)',text)[1])
    quantiles={k:re.search(r'^\s+'+k+r'%[ \t]+([0-9.]+[a-z]+)[ \t]*$',text,re.MULTILINE)[1] for k in ('50','99')}
    return dict(requests_per_second=rate,p50=quantiles['50'],p99=quantiles['99']),text,metrics,sockets

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--seconds',type=int,default=30)
    parser.add_argument('--workers',default=f'1,2,4,{os.cpu_count()}')
    parser.add_argument('--connections',default='100,1000')
    parser.add_argument('--output',default='bench/results')
    parser.add_argument('--download-mib',type=int,default=500)
    args=parser.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=True)
    env={}
    for name,cmd in [('kernel',['uname','-a']),('cpu',['lscpu']),('g++',['g++','--version']),('clang',['clang++','--version']),('cmake',['cmake','--version']),('os',['lsb_release','-a'])]:
        env[name]=subprocess.run(cmd,capture_output=True,text=True).stdout
    env['build']='release: -O3 -DNDEBUG -std=gnu++20';env['timestamp']=time.time()
    env['binary_sha256']=hashlib.sha256(BINARY.read_bytes()).hexdigest()
    env['source_commit']=subprocess.run(['git','rev-parse','HEAD'],capture_output=True,text=True).stdout.strip()
    env['parameters']=vars(args)
    (out/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
    results=[]
    with tempfile.TemporaryDirectory(prefix='zt-bench-') as temp:
        with backend(temp) as port:
            script=Path(temp)/'proxy.lua'
            script.write_text(f'wrk.path = "http://allowed.test:{port}/"\nwrk.headers["Host"] = "allowed.test:{port}"\n')
            for c in map(int,args.connections.split(',')):
                summary,raw,_,_=run_wrk(f'http://127.0.0.1:{port}/',c,args.seconds)
                results.append(dict(mode='direct',workers=0,connections=c,**summary));(out/f'wrk-direct-c{c}.txt').write_text(raw)
                print(results[-1],flush=True)
                for workers in dict.fromkeys(map(int,args.workers.split(','))):
                    with Proxy(Path(temp)/f'w{workers}-c{c}',port,workers=workers,max_connections=4096,max_per_client=4096,rate=1e8,burst=1e8,log_path='/dev/null',header_timeout_ms=10000,connect_timeout_ms=10000) as proxy:
                        summary,raw,metrics,sockets=run_wrk(f'http://127.0.0.1:{proxy.port}',c,args.seconds,script,proxy)
                        results.append(dict(mode='proxy',workers=workers,connections=c,**summary));name=f'wrk-w{workers}-c{c}'
                        (out/f'{name}.txt').write_text(raw);(out/f'{name}-metrics.txt').write_text(metrics)
                        (out/f'{name}-sockets.txt').write_text('\n'.join(line for line in sockets.splitlines() if f':{proxy.port} ' in line))
                        print(results[-1],flush=True)
            (out/'http.json').write_text(json.dumps(results,indent=2)+'\n')
        with backend(temp,tls=True) as port,Proxy(Path(temp)/'tls',port,idle_timeout_ms=60000,log_path='/dev/null') as proxy:
            size=args.download_mib*1024*1024
            def cpu_ticks():
                fields=Path(f'/proc/{proxy.process.pid}/stat').read_text().split()
                return int(fields[13])+int(fields[14])
            ticks=cpu_ticks();start=time.perf_counter();digest=hashlib.sha256();count=0
            with tls_context().wrap_socket(tunnel(proxy,port),server_hostname='allowed.test') as s:
                s.sendall(f'GET /?bytes={size} HTTP/1.1\r\nHost: allowed.test\r\nConnection: close\r\n\r\n'.encode())
                prefix=b''
                while b'\r\n\r\n' not in prefix:
                    prefix+=s.recv(16384)
                data=prefix.split(b'\r\n\r\n',1)[1];digest.update(data);count+=len(data)
                while True:
                    data=s.recv(65536)
                    if not data:
                        break
                    digest.update(data);count+=len(data)
            seconds=time.perf_counter()-start;cpu=(cpu_ticks()-ticks)/os.sysconf('SC_CLK_TCK')
            expected=hashlib.sha256();block=bytes(range(256))*256
            for _ in range(size//len(block)):
                expected.update(block)
            assert count==size and digest.digest()==expected.digest()
            summary=dict(bytes=count,seconds=seconds,mib_per_second=count/1024**2/seconds,proxy_cpu_percent_one_core=100*cpu/seconds,sha256=digest.hexdigest())
            (out/'tls.json').write_text(json.dumps(summary,indent=2)+'\n');print(summary,flush=True)
        with Proxy(Path(temp)/'idle',12345,workers=1,max_connections=4096,max_per_client=4096,header_timeout_ms=60000) as proxy:
            def rss():
                return int(Path(f'/proc/{proxy.process.pid}/status').read_text().split('VmRSS:')[1].split()[0])*1024
            before=rss();sockets=[]
            try:
                for _ in range(1000):
                    sockets.append(proxy.connect())
                time.sleep(.5)
                summary=dict(connections=1000,baseline_rss=before,total_rss=rss(),incremental_bytes_per_connection=(rss()-before)/1000)
                (out/'idle.json').write_text(json.dumps(summary,indent=2)+'\n');print(summary,flush=True)
            finally:
                for s in sockets:s.close()
    return 0
if __name__=='__main__':
    raise SystemExit(main())
