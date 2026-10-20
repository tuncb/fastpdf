"""Deterministic mutation smoke test; use an AddressSanitizer build.

This is bounded regression work, not a substitute for sustained coverage-guided
fuzzing. Seeds are the project's generated PDFs and JPEG 2000 fixtures.
"""
from pathlib import Path
import json
import os
import random
import subprocess
import sys

exe=Path(sys.argv[1]).resolve()
seeds=Path(sys.argv[2]).resolve()
root=Path(sys.argv[3]).resolve()
root.mkdir(parents=True,exist_ok=True)
count=int(sys.argv[4]) if len(sys.argv)>4 else 240
randomizer=random.Random(741918)
files=[p for p in seeds.glob('*.pdf') if not p.name.startswith('encrypted')]
failures=[]
timeouts=[]
for i in range(count):
    seed=files[i%len(files)]
    data=bytearray(seed.read_bytes())
    if i%4==0: data=data[:randomizer.randrange(len(data)+1)]
    elif i%4==1:
        for _ in range(randomizer.randrange(1,5)):
            at=randomizer.randrange(len(data));data[at]^=1<<randomizer.randrange(8)
    elif i%4==2:
        at=randomizer.randrange(len(data));data[at:at+randomizer.randrange(1,12)]=bytes([randomizer.choice([0,255,48,91,93,40,41])])*randomizer.randrange(1,40)
    else:
        at=data.find(b'stream\n')
        if at<0: at=0
        for _ in range(randomizer.randrange(1,10)):
            where=randomizer.randrange(at,len(data));data[where]=randomizer.randrange(256)
    source=root/f'{i:04d}-{seed.name}';source.write_bytes(data)
    try:
        result=subprocess.run([str(exe),str(source),'--render',str(root/'out.png'),'--scale','0.5'],capture_output=True,timeout=4)
        if result.returncode not in [0,2,3] or b'ERROR: AddressSanitizer' in result.stderr:
            (root/(source.stem+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
            failures.append({'file':source.name,'returncode':result.returncode})
    except subprocess.TimeoutExpired: timeouts.append(source.name)
report={'mutations':count,'crashes':failures,'timeouts':timeouts}
(root/'results.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report))
sys.exit(bool(failures or timeouts))
