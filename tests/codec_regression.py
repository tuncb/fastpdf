"""Check the owned codec against precomputed, independently decoded pixel fixtures."""
from pathlib import Path
import json
import base64
import hashlib
import struct
import subprocess
import sys
from jbig2_fixtures import generic_stream

executable=Path(sys.argv[1]).resolve()
output=Path(sys.argv[2]).resolve()
output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parent/'data'
cases=json.loads((root/'codecs.json').read_text())
for case in cases:
    name=case['name']
    target=output/(name+'.raw')
    result=subprocess.run([str(executable),str(root/(name+'.jp2')),str(target)],capture_output=True,text=True,timeout=20)
    assert result.returncode==0,(name,result.stderr)
    assert tuple(map(int,result.stdout.split()))==(case['width'],case['height'],case['components'])
    expected=(root/(name+'.raw')).read_bytes(); actual=target.read_bytes()
    assert len(expected)==len(actual),(name,len(expected),len(actual))
    maximum=max(abs(a-b) for a,b in zip(expected,actual))
    assert maximum<=case['tolerance'],(name,maximum)
    for reduced in case.get('reductions',[]):
        target=output/reduced['file']
        result=subprocess.run([str(executable),str(root/(name+'.jp2')),str(target),str(reduced['level'])],capture_output=True,text=True,timeout=20)
        assert result.returncode==0,(name,reduced['level'],result.stderr)
        assert tuple(map(int,result.stdout.split()))==(reduced['width'],reduced['height'],case['components'])
        expected=(root/reduced['file']).read_bytes(); actual=target.read_bytes()
        assert len(expected)==len(actual),(name,reduced['level'],len(expected),len(actual))
        maximum=max(abs(a-b) for a,b in zip(expected,actual))
        assert maximum<=case['tolerance'],(name,reduced['level'],maximum)
print(f"{len(cases)} JPEG 2000 pixel regressions passed")
print(f"{sum(len(case.get('reductions',[])) for case in cases)} reduced-resolution pixel regressions passed")
data=(root/'L-lossless.jp2').read_bytes()
stream=data[data.index(b'\xff\x4f'):]
variants={}
huge=bytearray(stream);struct.pack_into('>I',huge,huge.index(b'\xff\x51')+22,0x7fffffff)
variants['oversized-tile']=huge
missing=bytearray(stream);at=missing.index(b'\xff\x90');end=missing.index(b'\xff\x93')+2
struct.pack_into('>I',missing,at+6,end-at)
variants['missing-packets']=missing[:end]+b'\xff\xd9'
for name,data in variants.items():
    source=output/(name+'.j2k');source.write_bytes(data)
    result=subprocess.run([str(executable),str(source),str(output/'invalid.raw')],capture_output=True,text=True,timeout=10)
    assert result.returncode==1,(name,result.returncode,result.stderr)
print(f"{len(variants)} malformed JPEG 2000 rejection checks passed")

cases = json.loads((root / 'jbig2-generic.json').read_text())
for case in cases:
    source = output / 'generic.jb2'
    target = output / 'generic.raw'
    source.write_bytes(generic_stream(case['width'], case['height'], case['typical'], case['custom']))
    result = subprocess.run([str(executable), '--jbig2', str(source), str(target),
                             str(case['width']), str(case['height'])], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (case, result.stderr)
    actual = target.read_bytes()
    assert len(actual) == case['width'] * case['height'], case
    assert hashlib.sha256(actual).hexdigest() == case['sha256'], case
print(f"{len(cases)} JBIG2 arithmetic pixel regressions passed")

# Generated bit streams use every T.6 mode and runs longer than 2560 pixels.
# Expected pixels were checked independently with MuPDF before recording them.
cases = json.loads((root / 'fax-aligned.json').read_text())
for case in cases:
    source = output / 'aligned.fax'
    target = output / 'aligned.raw'
    source.write_bytes(base64.b64decode(case['data']))
    result = subprocess.run([str(executable), '--mmr-aligned', str(source), str(target),
                             str(case['width']), str(case['height'])], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (case['width'], result.stderr)
    actual = target.read_bytes()
    assert len(actual) == case['width'] * case['height'], case['width']
    assert hashlib.sha256(actual).hexdigest() == case['sha256'], case['width']
for data, width, height in [(b'', 1, 1), (b'\0', 17, 1), (b'\x80', 17, 2),
                             (b'\x20\x00\x00', 1, 1), (b'\x80', 0xffffffff, 0xffffffff)]:
    source.write_bytes(data)
    result = subprocess.run([str(executable), '--mmr-aligned', str(source), str(target), str(width), str(height)],
                             capture_output=True, text=True, timeout=10)
    assert result.returncode == 1, (data, width, height, result.stderr)
print(f"{len(cases)} aligned fax pixel regressions and 5 rejection checks passed")
