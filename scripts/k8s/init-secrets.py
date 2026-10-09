#!/usr/bin/env python3
"""Generate local-only secrets; never put them in Helm values or command arguments."""
from pathlib import Path
import base64
import json
import os
import secrets
import subprocess
ROOT=Path(__file__).resolve().parents[2]
folder=ROOT/'artifacts/secrets';folder.mkdir(parents=True,exist_ok=True);folder.chmod(0o700)
for name in ['proxy-token','policy-admin-token','policy-read-token','redis-password','grafana-password']:
    path=folder/name
    if not path.exists():
        fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'w') as f:f.write(secrets.token_hex(32)+'\n')
for namespace in ['zt-system','apps','monitoring']:
    resource={'apiVersion':'v1','kind':'Namespace','metadata':{'name':namespace}}
    subprocess.run(['kubectl','apply','-f','-'],input=json.dumps(resource),text=True,check=True)
    keys={'zt-system':['proxy-token','policy-admin-token','policy-read-token','redis-password'],
          'apps':['proxy-token'],'monitoring':['grafana-password']}[namespace]
    data={k:base64.b64encode((folder/k).read_bytes().strip()).decode() for k in keys}
    if namespace=='monitoring':data['admin-user']=base64.b64encode(b'admin').decode();data['admin-password']=data.pop('grafana-password')
    resource={'apiVersion':'v1','kind':'Secret','metadata':{'name':'zt-secrets','namespace':namespace},'type':'Opaque','data':data}
    subprocess.run(['kubectl','apply','-f','-'],input=json.dumps(resource),text=True,check=True)
