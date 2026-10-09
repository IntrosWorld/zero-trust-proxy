#!/usr/bin/env python3
"""Build and validate the packaged product using the real local integration suite.

Run with Docker access (normally sudo python3 scripts/check_docker.py).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]

def docker(*args,**kwargs):
    return subprocess.run(['docker',*args],check=True,text=True,**kwargs)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--image',default='zt-proxy:local')
    parser.add_argument('--skip-build',action='store_true')
    args=parser.parse_args()
    out=ROOT/'bench/results';out.mkdir(parents=True,exist_ok=True)
    report={'timestamp':time.time(),'image':args.image,'tests_passed':False}
    run_id=uuid.uuid4().hex
    try:
        if not args.skip_build:
            with (out/'docker-build.txt').open('w') as log:
                docker('build','--progress=plain','-f','deploy/Dockerfile','-t',args.image,'.',cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        print('Image built; checking identity, configuration and all integration cases.',flush=True)
        inspect=json.loads(docker('image','inspect',args.image,capture_output=True).stdout)[0]
        assert inspect['Config']['User']=='10001:10001',inspect['Config']['User']
        report.update(image_id=inspect['Id'],image_size_bytes=inspect['Size'],runtime_user=inspect['Config']['User'])
        version=docker('run','--rm',args.image,'--version',capture_output=True).stdout.strip()
        report['proxy_version']=version
        docker('run','--rm',args.image,'--config','/etc/zt-proxy/config.json','--check-config')
        with tempfile.TemporaryDirectory(prefix='zt-docker-tests-') as temp:
            env=os.environ.copy()
            env.update(ZT_BINARY=str(ROOT/'tests/integration/docker_binary.py'),ZT_DOCKER_IMAGE=args.image,
                       ZT_DOCKER_RUN_ID=run_id,ZT_DOCKER_TEST_ROOT=temp,TMPDIR=temp)
            env.pop('ZT_RUNNER',None)
            with (out/'docker-integration.txt').open('w') as log:
                result=subprocess.run([sys.executable,'-m','pytest','tests/integration','-q',f'--basetemp={temp}/tests'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
            report['integration_exit_code']=result.returncode
            report['integration_output']=(out/'docker-integration.txt').read_text()
            if result.returncode:
                raise RuntimeError(report['integration_output'])
            report['tests_passed']=True
        report['docker_version']=docker('version','--format','{{json .}}',capture_output=True).stdout.strip()
        report['storage']=json.loads(docker('info','--format','{{json .}}',capture_output=True).stdout)['DockerRootDir']
        (out/'docker-space.txt').write_text(docker('system','df','-v',capture_output=True).stdout)
        print(report['integration_output'],flush=True)
    finally:
        # Cleanup is limited to containers created by this exact test run.
        containers=docker('ps','-aq','--filter',f'label=zt-proxy.test-run={run_id}',capture_output=True).stdout.split()
        if containers:
            docker('rm','-f',*containers,stdout=subprocess.DEVNULL)
        (out/'docker.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
