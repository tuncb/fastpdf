from pathlib import Path
import json
import struct
import subprocess
import sys
import zlib
from fixtures import generate


def png_pixel(path,x,y):
    data=Path(path).read_bytes()
    assert data[:8]==b"\x89PNG\r\n\x1a\n"
    pos=8
    compressed=bytearray()
    width=height=channels=0
    while pos<len(data):
        length=struct.unpack_from(">I",data,pos)[0]
        name=data[pos+4:pos+8]
        chunk=data[pos+8:pos+8+length]
        if name==b"IHDR":
            width,height,depth,kind=struct.unpack_from(">IIBB",chunk)
            assert depth==8 and kind in (2,6), (depth,kind)
            channels=3 if kind==2 else 4
        elif name==b"IDAT": compressed+=chunk
        pos+=length+12
    raw=zlib.decompress(compressed)
    stride=width*channels
    previous=bytearray(stride)
    def paeth(a,b,c):
        p=a+b-c
        distances=[abs(p-a),abs(p-b),abs(p-c)]
        return [a,b,c][distances.index(min(distances))]
    for row in range(y+1):
        p=row*(stride+1)
        mode=raw[p]
        current=bytearray(raw[p+1:p+1+stride])
        for i in range(stride):
            a=current[i-channels] if i>=channels else 0
            b=previous[i]
            c=previous[i-channels] if i>=channels else 0
            delta=[0,a,b,(a+b)//2,paeth(a,b,c)][mode]
            current[i]=(current[i]+delta)&255
        previous=current
    return tuple(previous[x*channels:x*channels+3])


def run(executable,root):
    cases=generate(root)
    failures=[]
    for name,case in cases.items():
        output=root/f"{name}.png"
        args=[str(executable),str(root/f"{name}.pdf"),"--render",str(output),"--text"]
        if "password" in case: args += ["--password",case["password"]]
        try:
            result=subprocess.run(args,capture_output=True,text=True,encoding="utf-8",timeout=20)
            if case.get("error"):
                assert result.returncode==2,result.stdout+result.stderr
                continue
            assert result.returncode==0,result.stdout+result.stderr
            info=json.loads(result.stdout)
            assert info["pages"]==1,info
            assert info["renders"]==1,info
            if "text" in case: assert case["text"] in info["text"],info
            if "size" in case: assert (info["width"],info["height"])==tuple(case["size"]),info
            if case.get("repaired"): assert info["repaired"],info
            else: assert not info["repaired"],info
            if "pixel" in case:
                x,y,expected=case["pixel"]
                actual=png_pixel(output,x,y)
                assert all(abs(a-b)<=case.get("tolerance",1) for a,b in zip(actual,expected)),f"pixel {x},{y}: {actual} != {expected}; {info}"
            if not case.get("repaired"): assert not info["warnings"],info
            if "owner" in case:
                owner=subprocess.run([str(executable),str(root/f"{name}.pdf"),"--text","--password",case["owner"]],capture_output=True,text=True,encoding="utf-8",timeout=20)
                assert owner.returncode==0,owner.stderr
                assert case["text"] in json.loads(owner.stdout)["text"],owner.stdout
                wrong=subprocess.run([str(executable),str(root/f"{name}.pdf"),"--password","wrong"],capture_output=True,text=True,encoding="utf-8",timeout=20)
                assert wrong.returncode==2,"Incorrect password was accepted"
        except Exception as error:
            failures.append(f"{name}: {error}")
    if failures:
        print("\n".join(failures))
        raise SystemExit(f"{len(failures)}/{len(cases)} integration fixtures failed")
    print(f"{len(cases)} PDF fixtures passed (rendered pixels, text, structure, filters, encryption, and rejection cases)")


if __name__=="__main__":
    run(Path(sys.argv[1]),Path(sys.argv[2]))
