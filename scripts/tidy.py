#!/usr/bin/env python3
from pathlib import Path
import concurrent.futures
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
def check(path):
    p=subprocess.run(['clang-tidy','-p',str(root/'build/debug'),str(path)],capture_output=True,text=True)
    return p.returncode,p.stdout+p.stderr
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    results=list(pool.map(check,sorted((root/'src').rglob('*.cpp'))))
for code,text in results:
    if code:
        print(text)
print(f'clang-tidy: {len(results)} translation units, {sum(bool(code) for code,_ in results)} failures')
sys.exit(any(code for code,_ in results))
