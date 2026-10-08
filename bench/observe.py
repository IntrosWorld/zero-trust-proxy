#!/usr/bin/env python3
"""Compare the active gauge to accepted server-side ESTAB sockets at rest."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/integration'))
from support import Proxy
out=ROOT/'bench/results'
with tempfile.TemporaryDirectory(prefix='zt-observe-') as temp,Proxy(temp,12345,workers=2,header_timeout_ms=10000) as proxy:
    clients=[proxy.connect() for _ in range(100)]
    try:
        for _ in range(100):
            metrics=proxy.admin_get().decode()
            active=int(re.search(r'^zt_active_connections (\d+)$',metrics,re.M)[1])
            if active==100:
                break
            time.sleep(.01)
        sockets=subprocess.run(['ss','-Htnp','state','established',f'( sport = :{proxy.port} )'],capture_output=True,text=True,check=True).stdout
        count=len(sockets.splitlines())
        assert active==count==100,(active,count)
        report=dict(active_metric=active,accepted_established_sockets=count,match=True,port=proxy.port,pid=proxy.process.pid)
        (out/'observability.json').write_text(json.dumps(report,indent=2)+'\n')
        (out/'observability-metrics.txt').write_text(metrics)
        (out/'observability-sockets.txt').write_text(sockets)
        print('Active connections match: metrics=100, ss accepted ESTAB=100')
    finally:
        for client in clients:
            client.close()
