#!/usr/bin/env python3
"""Capture an actual openssl s_client ClientHello on a local TCP socket."""
import socket
import subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with socket.socket() as listener:
    listener.bind(('127.0.0.1',0))
    listener.listen()
    process=subprocess.Popen(['openssl','s_client','-connect',f'127.0.0.1:{listener.getsockname()[1]}','-servername','allowed.test'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        with listener.accept()[0] as peer:
            data=b''
            while len(data)<5:
                data+=peer.recv(5-len(data))
            length=int.from_bytes(data[3:5],'big')
            while len(data)<length+5:
                data+=peer.recv(length+5-len(data))
            (root/'tests/fuzz/corpus/sni/openssl-clienthello.bin').write_bytes(data)
            print(f'Captured {len(data)} bytes from openssl s_client')
    finally:
        process.terminate()
        process.wait()
