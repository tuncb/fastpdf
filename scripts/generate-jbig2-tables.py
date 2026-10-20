"""Transcribe the factual code assignments in ITU-T T.88 tables B.1-B.15.

Input: UTF-8 text pages extracted from the published ITU specification, using
pdfprobe --text --extract. The resulting numeric tables are checked in.
"""
from pathlib import Path
import re
import sys

root=Path(sys.argv[1])
tables=[]
negative={8:6,9:7,10:5,14:2,15:5}
for page in range(103,111):
    for line in root.joinpath(f'jbig2-{page}.txt').read_text(encoding='utf-8').splitlines():
        if re.match(r'VAL\s+PREFLEN\s+RANGELEN',line): tables.append([]); continue
        if not tables: continue
        match=re.match(r'^OOB\s+(\d+)\s+([01]+)\s*$',line)
        if match:
            length,code=match.groups(); tables[-1].append((int(length),0,0,False,True,int(code,2))); continue
        match=re.match(r'^(.+?)\s+(\d+)\s+(\d+)\s+([01]+)(?:\s+\+\s+(.+))?\s*$',line)
        if not match: continue
        val,length,extra,code,expression=match.groups()
        lower=False
        if expression:
            if expression.startswith('VAL'): base=0
            elif m:=re.match(r'\(VAL\s+\+\s+(\d+)\)',expression): base=-int(m[1])
            elif m:=re.match(r'\(VAL\s+[^\d]*(\d+)\)',expression): base=int(m[1])
            elif m:=re.match(r'\((\d+)\s+[^A-Z]*VAL\)',expression): base=-int(m[1]); lower=True
            else: raise ValueError(expression)
        else:
            base=int(val.strip())
            if len(tables[-1])<negative.get(len(tables),0): base=-base
        assert len(code)==int(length),(line,code,length)
        tables[-1].append((int(length),int(extra),base,lower,False,int(code,2)))
assert len(tables)==15
output=['// Numeric prefix/range assignments from ITU-T T.88, Annex B.\n']
output.append('static const std::array<Huffman,16> standard_tables={{\n{},\n')
for i,table in enumerate(tables,1):
    assert table,(i,table)
    output.append('{ // B.'+str(i)+'\n')
    for length,extra,base,lower,oob,code in table:
        output.append(f'    {{{length},{extra},{base},{str(lower).lower()},{str(oob).lower()},{code}}},\n')
    output.append('},\n')
output.append('}};\n')
Path(__file__).resolve().parents[1].joinpath('src/jbig2_tables.inc').write_text(''.join(output),encoding='utf-8')
print([len(t) for t in tables])
