"""Regenerate owned JPEG 2000 fixtures. Pillow/OpenJPEG is a development-only encoder."""
from pathlib import Path
import hashlib
import json
import random
import struct
from PIL import Image

root=Path(__file__).resolve().parents[1]/'tests'/'data'
root.mkdir(exist_ok=True)
cases=[]
rng=random.Random(0xFA57)
for mode in ['L','RGB','RGBA']:
    channels=len(mode) if mode!='L' else 1
    w,h=37,29
    raw=bytes((x*7+y*11+c*71)%256 for y in range(h) for x in range(w) for c in range(channels))
    im=Image.frombytes(mode,(w,h),raw)
    for lossless in [True,False]:
        cases.append((mode+('-lossless' if lossless else '-lossy'),im,{'num_resolutions':4,'irreversible':not lossless,'mct':int(mode=='RGB')},0 if lossless else 1))
im=Image.frombytes('RGB',(65,73),bytes(rng.randrange(256) for _ in range(65*73*3)))
for progression in ['LRCP','RLCP','RPCL','PCRL','CPRL']:
    cases.append(('tiled-'+progression,im,{'num_resolutions':3,'tile_size':(32,32),'precinct_size':(16,16),'codeblock_size':(8,8),'quality_mode':'rates','quality_layers':[6,2,1],'progression':progression},0))
im=Image.frombytes('I;16',(37,29),b''.join(struct.pack('<H',(x*2017+y*499)%65536) for y in range(29) for x in range(37)))
cases.append(('gray-16bit',im,{'num_resolutions':4},0))
manifest=[]
for name,image,options,tolerance in cases:
    file=root/(name+'.jp2'); image.save(file,format='JPEG2000',**options)
    decoded=Image.open(file)
    raw=bytes((v[0]*255+32767)//65535 for v in struct.iter_unpack('<H',decoded.tobytes())) if decoded.mode=='I;16' else decoded.tobytes()
    (root/(name+'.raw')).write_bytes(raw)
    manifest.append({'name':name,'width':image.width,'height':image.height,'components':len(decoded.getbands()),'tolerance':tolerance,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()})
# Reinterpret centered coefficients as signed samples, then compare display
# normalization with the independent decoder. Use the raw codestream container.
data=(root/'L-lossless.jp2').read_bytes()
data=bytearray(data[data.index(b'\xff\x4f'):]);data[data.index(b'\xff\x51')+40]|=128
file=root/'gray-signed.jp2';file.write_bytes(data)
decoded=Image.open(file);(root/'gray-signed.raw').write_bytes(decoded.tobytes())
manifest.append({'name':'gray-signed','width':decoded.width,'height':decoded.height,'components':1,'tolerance':0,'sha256':hashlib.sha256(data).hexdigest()})
for case in manifest:
    decoded=Image.open(root/(case['name']+'.jp2'))
    decoded.reduce=1
    raw=decoded.tobytes()
    if decoded.mode=='I;16': raw=bytes((v[0]*255+32767)//65535 for v in struct.iter_unpack('<H',raw))
    file=case['name']+'-r1.raw'
    (root/file).write_bytes(raw)
    case['reductions']=[{'level':1,'width':decoded.width,'height':decoded.height,'file':file}]
(root/'codecs.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('Generated',len(manifest),'owned JPEG 2000 fixtures')
