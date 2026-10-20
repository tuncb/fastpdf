"""Optional development differential test. Pillow/OpenJPEG is only a test oracle.

The shipped executables and regular CTest tests do not depend on Pillow.
"""
from pathlib import Path
import json
import random
import subprocess
import sys
from PIL import Image


def run(executable, root):
    root.mkdir(parents=True, exist_ok=True)
    randomizer=random.Random(59319)
    cases=[]
    for mode in ('L','RGB'):
        for size in ((2,2),(17,13),(64,64),(128,77),(257,259)):
            w,h=size
            for pattern in ('gradient','noise'):
                count=1 if mode=='L' else 3
                samples=bytes(randomizer.randrange(256) if pattern=='noise' else (x*7+y*11+c*53)%256
                              for y in range(h) for x in range(w) for c in range(count))
                image=Image.frombytes(mode,size,samples)
                for irreversible in (False,True):
                    options={'num_resolutions':min(4, min(w,h).bit_length()),'irreversible':irreversible}
                    if mode=='RGB': options['mct']=1
                    cases.append((f'{mode}-{w}x{h}-{pattern}-{irreversible}',image,options))
    image=Image.frombytes('RGB',(257,193),bytes(randomizer.randrange(256) for _ in range(257*193*3)))
    for progression in ('LRCP','RLCP','RPCL','PCRL','CPRL'):
        for tiled in (False,True):
            options={'num_resolutions':4,'progression':progression,'quality_mode':'rates','quality_layers':[8,3,1], 'precinct_size':(64,64), 'codeblock_size':(32,32)}
            if tiled: options['tile_size']=(128,128)
            cases.append((f'progression-{progression}-{tiled}',image,options))
    image=Image.frombytes('RGB',(128,96),bytes(randomizer.randrange(256) for _ in range(128*96*3)))
    for offset in ((0,0),(3,5)):
        for irreversible in (False,True):
            cases.append((f'offset-{offset[0]}-{offset[1]}-{irreversible}',image,
                          {'num_resolutions':3,'offset':offset,'tile_size':(41,37),
                           'irreversible':irreversible,'mct':1}))
    results=[]
    for name,image,options in cases:
        source=root/(name+'.jp2')
        image.save(source,format='JPEG2000',**options)
        reductions=[0,1]
        # Pillow rounds its output dimensions differently from JPEG 2000's ceil
        # coordinates beyond reduction 1; use aligned dimensions for that oracle.
        if options['num_resolutions']>2 and all(n%4==0 for n in image.size): reductions.append(2)
        # Pillow's reduced decoder rejects nonzero image origins. Full-resolution
        # offset cases still exercise the halo and reversible lifting changes.
        if options.get('offset',(0,0)) != (0,0): reductions=[0]
        for reduction in reductions:
            oracle=Image.open(source)
            oracle.reduce=reduction
            reference=oracle.tobytes()
            output=root/(name+f'-r{reduction}.raw')
            result=subprocess.run([str(executable),str(source),str(output),str(reduction)],capture_output=True,text=True,timeout=30)
            report={'name':name,'reduction':reduction,'returncode':result.returncode,
                    'tolerance':1 if options.get('irreversible') else 0}
            if result.returncode: report['error']=result.stderr.strip()
            else:
                decoded=output.read_bytes()
                assert tuple(map(int,result.stdout.split()))==(*oracle.size,len(oracle.getbands())),(name,reduction,result.stdout,oracle.size)
                assert len(reference)==len(decoded),(name,reduction,len(reference),len(decoded))
                errors=[abs(a-b) for a,b in zip(decoded,reference)]
                report.update(max_error=max(errors),mean_error=sum(errors)/len(errors),different=sum(e!=0 for e in errors))
            results.append(report)
            print(json.dumps(report),flush=True)
    (root/'results.json').write_text(json.dumps(results,indent=2))
    return 0 if all(x.get('max_error',1000)<=x['tolerance'] for x in results) else 1

if __name__=='__main__':
    sys.exit(run(Path(sys.argv[1]).resolve(),Path(sys.argv[2]).resolve()))
