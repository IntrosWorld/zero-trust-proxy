#!/usr/bin/env python3
"""Test-only binary adapter: run each existing integration fixture in Docker."""
import json
import os
from pathlib import Path
import sys
import uuid

image=os.environ.get('ZT_DOCKER_IMAGE','zt-proxy:local')
run_id=os.environ['ZT_DOCKER_RUN_ID']
root=Path(os.environ['ZT_DOCKER_TEST_ROOT']).resolve()
args=sys.argv[1:]
command=['docker','run','--rm','--name',f'zt-test-{uuid.uuid4().hex[:12]}',
         '--label',f'zt-proxy.test-run={run_id}','--network','host','--read-only',
         '--cap-drop','ALL','--security-opt','no-new-privileges','--memory','256m','--pids-limit','64']
flag = '--config' if '--config' in args else '--validate-policy' if '--validate-policy' in args else None
if flag:
    config=Path(args[args.index(flag)+1]).resolve()
    config.relative_to(root)
    config.chmod(0o644)
    current=config.parent
    while current != root:
        current.chmod(0o755)
        current=current.parent
    root.chmod(0o755)
    config.parent.chmod(0o777)
    command+=['--mount',f'type=bind,src={root},dst={root}']
    try:
        settings=json.loads(config.read_text())
        command+=['--env',f"ZT_ADMIN_PORT={settings.get('admin_port',9100)}",
                  '--env',f"ZT_ADMIN_HOST={settings.get('admin','127.0.0.1')}"]
    except (ValueError,AttributeError):
        pass # Let the actual proxy report deliberately malformed test configs.
command += [image]+args
# Docker forwards SIGINT/SIGTERM/SIGHUP from the fixture to the container PID 1.
os.execvp(command[0],command)
