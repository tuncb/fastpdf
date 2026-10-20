"""Render all pages of supplied local PDFs and record the actual results."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import time

exe=Path(sys.argv[1]).resolve()
output=Path(sys.argv[2]).resolve()
output.mkdir(parents=True,exist_ok=True)
files=[]
for argument in sys.argv[3:]:
    path=Path(argument).resolve()
    files.extend(sorted(path.glob('*.pdf')) if path.is_dir() else [path])
results=[]
for source in files:
    begin=time.perf_counter()
    try:
        proc=subprocess.run([str(exe),str(source),'--all','--render',str(output/(source.stem+'.png'))],capture_output=True,encoding='utf-8',timeout=180)
        report=json.loads(proc.stdout if proc.returncode in (0,3) else proc.stderr)
        report.update(file=source.name,returncode=proc.returncode,wall_seconds=round(time.perf_counter()-begin,3),sha256=hashlib.sha256(source.read_bytes()).hexdigest())
    except Exception as error: report={'file':source.name,'error':str(error),'returncode':-1}
    results.append(report)
    print(json.dumps(report),flush=True)
(output/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
sys.exit(any(r['returncode'] not in (0,3) for r in results))
