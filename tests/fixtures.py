"""Deterministic PDF conformance fixtures built with the Python standard library.

These are test data generators, not a runtime dependency of FastPDF.
"""
from pathlib import Path
import base64
import hashlib
import random
import struct
import zlib


def stream(data, attributes=b"", compression=False):
    if isinstance(data, str):
        data = data.encode("ascii")
    if compression:
        data = zlib.compress(data)
        attributes += b" /Filter /FlateDecode"
    return b"<< /Length " + str(len(data)).encode() + b" " + attributes + b" >>\nstream\n" + data + b"\nendstream"


def pdf(objects, trailer=b"", root=1):
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode() + obj + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects)+1}\n0000000000 65535 f \n".encode()
    for offset in offsets[1:]:
        out += f"{offset:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objects)+1} /Root {root} 0 R ".encode() + trailer
    out += f">>\nstartxref\n{xref}\n%%EOF\n".encode()
    return bytes(out)


TEXT = b"BT /F1 20 Tf 1 0 0 1 20 175 Tm (FastPDF hello world) Tj ET\n"
CONTENT = b"1 0 0 rg 20 20 100 80 re f\n0 g\n" + TEXT


def base(content=CONTENT, resources=b"", extra_page=b"", extra_catalog=b"", extra_pages=b""):
    return [
        b"<< /Type /Catalog /Pages 2 0 R " + extra_catalog + b" >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 " + extra_pages + b" >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 220] /Resources << /Font << /F1 4 0 R >> " + resources + b" >> /Contents 5 0 R " + extra_page + b" >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream(content),
    ]


def lzw_encode(data, early=1):
    table = {bytes([i]): i for i in range(256)}
    next_code = 258
    codes = [256]
    previous = b""
    for byte in data:
        trial = previous + bytes([byte])
        if trial in table:
            previous = trial
        else:
            codes.append(table[previous])
            if next_code < 4096:
                table[trial] = next_code
                next_code += 1
            previous = bytes([byte])
    if previous:
        codes.append(table[previous])
    codes.append(257)
    # Pack according to the decoder's dictionary growth, independently of the encoder.
    width, next_code, had_previous = 9, 258, False
    bits = []
    for code in codes:
        bits.extend((code >> i) & 1 for i in range(width - 1, -1, -1))
        if code == 256:
            width, next_code, had_previous = 9, 258, False
        elif code != 257:
            if had_previous and next_code < 4096:
                next_code += 1
                if width < 12 and next_code + early == 1 << width:
                    width += 1
            had_previous = True
    bits += [0] * (-len(bits) % 8)
    return bytes(sum(bits[i+j] << (7-j) for j in range(8)) for i in range(0, len(bits), 8))


def object_stream_pdf(hybrid=False):
    objects = base()
    compressed = objects[:4]
    body = bytearray()
    header = bytearray()
    for i, obj in enumerate(compressed, 1):
        header += f"{i} {len(body)} ".encode()
        body += obj + b" "
    obj_stream = stream(header + body, f"/Type /ObjStm /N 4 /First {len(header)}".encode(), True)
    out = bytearray(b"%PDF-1.7\n")
    offsets = {}
    for number, obj in [(5, objects[4]), (6, obj_stream)]:
        offsets[number] = len(out)
        out += f"{number} 0 obj\n".encode() + obj + b"\nendobj\n"
    offsets[7] = len(out)
    entries = bytearray(struct.pack(">BIH", 0, 0, 65535))
    for i in range(1, 8):
        entries += struct.pack(">BIH", 2, 6, i-1) if i <= 4 else struct.pack(">BIH", 1, offsets[i], 0)
    out += b"7 0 obj\n" + stream(entries, b"/Type /XRef /Size 8 /Root 1 0 R /W [1 4 2]", True)
    out += f"\nendobj\nstartxref\n{offsets[7]}\n%%EOF\n".encode()
    if hybrid:
        position=len(out)
        out+=b"xref\n0 8\n0000000000 65535 f \n"+b"0000000000 00000 f \n"*4
        for i in [5,6,7]: out+=f"{offsets[i]:010d} 00000 n \n".encode()
        out+=f"trailer\n<< /Size 8 /Root 1 0 R /XRefStm {offsets[7]} >>\nstartxref\n{position}\n%%EOF\n".encode()
    return bytes(out)


def jbig2_huffman_fixture():
    def packed(value):
        value+='0'*(-len(value)%8)
        return bytes(int(value[i:i+8],2) for i in range(0,len(value),8))
    def segment(number,kind,page,data,refs=()):
        return struct.pack('>IBB',number,kind,len(refs)<<5)+bytes(refs)+struct.pack('>BI',page,len(data))+data
    # Two original 5-by-8 glyphs, in an uncompressed collective bitmap inside
    # a Huffman symbol dictionary. These pixels and streams are project-owned.
    a=[14,17,17,31,17,17,17,0]
    b=[30,17,17,30,17,17,30,0]
    raw=b''.join(((left<<11)|(right<<6)).to_bytes(2,'big') for left,right in zip(a,b))
    dictionary=struct.pack('>HII',1,2,2)+packed('1110'+'100'+'1110'+'010'+'0'+'111111'+'00000')+raw+packed('00000'+'00010')
    globals=segment(0,0,0,dictionary)
    page=segment(1,48,1,struct.pack('>IIIIBH',32,24,0,0,1,0))
    ids=packed(''.join(f'{int(i==1):04b}' for i in range(35))+'00')
    text=struct.pack('>IIIIBHHI',28,20,2,2,0,17,0,2)+ids+packed('0'+'0'+'00'+'0000000'+'0'+'100'+'0000'+'1'+'01')
    data=page+segment(2,7,1,text,(0,))
    objects=base(b'q 128 0 0 96 20 20 cm /Im Do Q',b'/XObject << /Im 6 0 R >>')
    objects += [stream(data,b'/Type /XObject /Subtype /Image /Width 32 /Height 24 /ColorSpace /DeviceGray /BitsPerComponent 1 /Interpolate false /Filter /JBIG2Decode /DecodeParms << /JBIG2Globals 7 0 R >>'),stream(globals)]
    return pdf(objects)


def jbig2_streamed_fixture(random_access=False):
    # A black 8x8 page with two white rows. The region advertises eight rows;
    # its unknown-length terminator supplies the true row count of two.
    # MMR: two vertical-zero codes, EOFB, and byte alignment.
    encoded='11'+'000000000001000000000001'
    encoded+='0'*(-len(encoded)%8)
    mmr=bytes(int(encoded[i:i+8],2) for i in range(0,len(encoded),8))
    data=[struct.pack('>IIIIBH',8,8,0,0,68,0),
          struct.pack('>IIIIBB',8,8,0,0,4,1)+mmr+b'\0\0'+struct.pack('>I',2),b'',b'']
    headers=[struct.pack('>IBBBI',i+1,kind,0,1 if kind!=51 else 0,0xffffffff if kind==38 else len(body))
             for i,(kind,body) in enumerate(zip((48,38,49,51),data))]
    payload=(b'\x97JB2\r\n\x1a\n\x02'+b''.join(headers)+b''.join(data)) if random_access else b''.join(h+b for h,b in zip(headers,data))
    objects=base(b'q 128 0 0 128 20 20 cm /Im Do Q',b'/XObject << /Im 6 0 R >>')
    objects.append(stream(payload,b'/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceGray /BitsPerComponent 1 /Interpolate false /Filter /JBIG2Decode'))
    return pdf(objects)


PAD = bytes.fromhex("28bf4e5e4e758a4164004e56fffa01082e2e00b6d0683e802f0ca9fe6453697a")
FILE_ID = b"FastPDF-test-file"


def rc4(data, key):
    state = list(range(256))
    j = 0
    for i in range(256):
        j = (j + state[i] + key[i % len(key)]) & 255
        state[i], state[j] = state[j], state[i]
    i = j = 0
    out = bytearray()
    for byte in data:
        i = (i + 1) & 255
        j = (j + state[i]) & 255
        state[i], state[j] = state[j], state[i]
        out.append(byte ^ state[(state[i] + state[j]) & 255])
    return bytes(out)


def aes(data, key, iv=b"", padding=False):
    # Windows CNG provides an independent way to manufacture encrypted test data.
    import ctypes as C
    from ctypes import wintypes as W
    lib = C.WinDLL("bcrypt")
    handle = C.c_void_p()
    key_handle = C.c_void_p()
    def check(value):
        if value < 0:
            raise RuntimeError(f"CNG error {value & 0xffffffff:08x}")
    check(lib.BCryptOpenAlgorithmProvider(C.byref(handle), C.c_wchar_p("AES"), None, 0))
    try:
        mode = C.create_unicode_buffer("ChainingModeCBC" if iv else "ChainingModeECB")
        check(lib.BCryptSetProperty(handle, C.c_wchar_p("ChainingMode"), mode, C.sizeof(mode), 0))
        key_buffer = C.create_string_buffer(key)
        check(lib.BCryptGenerateSymmetricKey(handle, C.byref(key_handle), None, 0, key_buffer, len(key), 0))
        source = C.create_string_buffer(data)
        dest = C.create_string_buffer(len(data) + 32)
        iv_buffer = C.create_string_buffer(iv) if iv else None
        done = W.ULONG()
        check(lib.BCryptEncrypt(key_handle, source, len(data), None, iv_buffer, len(iv), dest, len(dest), C.byref(done), 1 if padding else 0))
        return dest.raw[:done.value]
    finally:
        if key_handle:
            lib.BCryptDestroyKey(key_handle)
        lib.BCryptCloseAlgorithmProvider(handle, 0)


def hash_r6(password, salt, user=b"", revision=6):
    key = hashlib.sha256(password + salt + user).digest()
    if revision == 5:
        return key
    count = 0
    encrypted = b"\xff"
    while count < 64 or encrypted[-1] > count - 32:
        encrypted = aes((password + key + user) * 64, key[:16], key[16:32])
        key = [hashlib.sha256, hashlib.sha384, hashlib.sha512][sum(encrypted[:16]) % 3](encrypted).digest()
        count += 1
    return key[:32]


def encrypted_pdf(revision, password=b"secret", owner=b"owner", content=CONTENT):
    permissions = -4
    if revision <= 4:
        length = 5 if revision == 2 else 16
        padded = lambda value: (value + PAD)[:32]
        owner_key = hashlib.md5(padded(owner)).digest()
        if revision >= 3:
            for _ in range(50):
                owner_key = hashlib.md5(owner_key).digest()
        owner_key = owner_key[:length]
        o = rc4(padded(password), owner_key)
        if revision >= 3:
            for i in range(1, 20):
                o = rc4(o, bytes(v ^ i for v in owner_key))
        key = hashlib.md5(padded(password) + o + struct.pack("<i", permissions) + FILE_ID).digest()
        if revision >= 3:
            for _ in range(50):
                key = hashlib.md5(key[:length]).digest()
        key = key[:length]
        u = rc4(PAD, key) if revision == 2 else rc4(hashlib.md5(PAD + FILE_ID).digest(), key)
        if revision >= 3:
            for i in range(1, 20):
                u = rc4(u, bytes(v ^ i for v in key))
            u += b"\0" * 16
        extra = b""
        if revision == 4:
            extra = b" /CF << /StdCF << /CFM /AESV2 /Length 16 >> >> /StmF /StdCF /StrF /StdCF"
        enc = f"<< /Filter /Standard /V {1 if revision == 2 else 2 if revision == 3 else 4} /R {revision} /Length {length*8} /P {permissions} /O <{o.hex()}> /U <{u.hex()}>".encode() + extra + b" >>"
        def encrypt(data, number):
            material = key + struct.pack("<I", number)[:3] + b"\0\0" + (b"sAlT" if revision == 4 else b"")
            object_key = hashlib.md5(material).digest()[:min(length+5, 16)]
            iv = bytes(range(16))
            return iv + aes(data, object_key, iv, True) if revision == 4 else rc4(data, object_key)
    else:
        key = bytes(range(32))
        uvs, uks, ovs, oks = b"12345678", b"ABCDEFGH", b"87654321", b"HGFEDCBA"
        u = hash_r6(password, uvs, revision=revision) + uvs + uks
        o = hash_r6(owner, ovs, u, revision) + ovs + oks
        ue = aes(key, hash_r6(password, uks, revision=revision), bytes(16))
        oe = aes(key, hash_r6(owner, oks, u, revision), bytes(16))
        perms = aes(struct.pack("<i", permissions) + b"\xff"*4 + b"Tadb1234", key)
        enc = f"<< /Filter /Standard /V 5 /R {revision} /Length 256 /P {permissions} /O <{o.hex()}> /U <{u.hex()}> /OE <{oe.hex()}> /UE <{ue.hex()}> /Perms <{perms.hex()}> /CF << /StdCF << /CFM /AESV3 /Length 32 >> >> /StmF /StdCF /StrF /StdCF >>".encode()
        def encrypt(data, number):
            iv = bytes(range(16))
            return iv + aes(data, key, iv, True)
    objects = base()
    objects[4] = stream(encrypt(zlib.compress(content), 5), b"/Filter /FlateDecode")
    objects += [enc]
    return pdf(objects, f"/Encrypt 6 0 R /ID [<{FILE_ID.hex()}> <{FILE_ID.hex()}>]".encode())


def t1_encrypt(data, seed):
    out = bytearray()
    for b in data:
        c = b ^ (seed >> 8)
        seed = ((c + seed) * 52845 + 22719) & 65535
        out.append(c)
    return bytes(out)


def char_number(n):
    if -107 <= n <= 107:
        return bytes([n + 139])
    if 108 <= n <= 1131:
        n -= 108
        return bytes([247 + n // 256, n % 256])
    if -1131 <= n <= -108:
        n = -n - 108
        return bytes([251 + n // 256, n % 256])
    return b"\xff" + struct.pack(">i", n)


def type1_fixture():
    cs = bytearray(b"abcd")
    for args, op in [([0, 600], 13), ([50, 0], 21), ([250, 700, 250, -700, -500, 0], 5), ([], 9), ([], 14)]:
        for n in args:
            cs += char_number(n)
        cs.append(op)
    cs = t1_encrypt(cs, 4330)
    header = b"%!PS-AdobeFont-1.0: FastPDFTest 1.0\n/FontName /FastPDFTest def\n/FontType 1 def\n/FontMatrix [0.001 0 0 0.001 0 0] def\n/FontBBox [0 0 600 700] def\n/Encoding 256 array dup 65 /A put def\ncurrentfile eexec\n"
    private = b"abcd/Private 8 dict dup begin /lenIV 4 def /Subrs 0 array def /CharStrings 1 dict dup begin /A " + str(len(cs)).encode() + b" RD " + cs + b" ND end end closefile"
    program = header + t1_encrypt(private, 55665)
    objects = base(b"BT /F1 100 Tf 1 0 0 1 20 50 Tm (AAA) Tj ET")
    objects[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /FastPDFTest /FirstChar 65 /LastChar 65 /Widths [600] /FontDescriptor 6 0 R >>"
    objects += [b"<< /Type /FontDescriptor /FontName /FastPDFTest /Flags 32 /FontBBox [0 0 600 700] /Ascent 700 /Descent 0 /CapHeight 700 /StemV 80 /FontFile 7 0 R >>", stream(program, f"/Length1 {len(header)} /Length2 {len(program)-len(header)} /Length3 0".encode(), True)]
    return pdf(objects)


def generate(directory):
    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    cases = {}
    def add(name, data, **expected):
        (root / f"{name}.pdf").write_bytes(data)
        cases[name] = expected
    add("plain", pdf(base()), text="FastPDF hello world", pixel=(50, 150, (255, 0, 0)))
    objects = base()
    objects[4] = stream(b"BT\n" + b"0 0 Td\n" * 700000 + b"ET\n" + CONTENT, compression=True)
    add("long_content", pdf(objects), text="FastPDF hello world", pixel=(50, 150, (255, 0, 0)))
    for count in range(1000):
        encoded = zlib.compress(CONTENT + b"%" + b"a" * count + b"\n")
        if encoded[-1] == 10:
            break
    else:
        raise AssertionError("Cannot construct a newline checksum fixture")
    objects = base()
    objects[4] = stream(encoded[:-1], b"/Filter /FlateDecode")
    add("flate_missing_eol_checksum", pdf(objects), text="FastPDF hello world",
        pixel=(50, 150, (255, 0, 0)), warnings=["Recovered a truncated Flate stream (object 5)"])
    objects[4] = stream(encoded[:-4], b"/Filter /FlateDecode")
    add("flate_missing_checksum", pdf(objects), error=True)
    inline_mask = b"BI /W 8 /H 8 /IM true /BPC 1 ID " + b"\x00" * 8 + b" EI"
    inverse_mask = inline_mask.replace(b"/BPC 1", b"/BPC 1 /D [1 0]")
    content = (b"q 1 0 0 rg 40 0 0 40 20 20 cm " + inline_mask + b" Q\n" +
               b"q 0 0 1 rg 40 0 0 40 80 20 cm " + inline_mask + b" Q\n" +
               b"q 1 0 0 rg 40 0 0 40 140 20 cm " + inverse_mask + b" Q\n")
    add("inline_stencil_reuse", pdf(base(content)),
        pixels=[(40, 180, (255, 0, 0)), (100, 180, (0, 0, 255)), (160, 180, (255, 255, 255))])
    for cap in (0, 1, 2):
        content = f"20 w {cap} J 50 50 m 50 50 l s 100 50 m 100 50 l S 150 50 m h S 200 50 m S ".encode()
        # The last stroke contains a degenerate and a non-degenerate subpath.
        content += b"50 100 m 50 100 l h 100 100 m 130 100 l S"
        dot = (0, 0, 0) if cap == 1 else (255, 255, 255)
        add(f"degenerate_stroke_{cap}", pdf(base(content)), pixels=[(50, 170, dot), (100, 170, dot),
            (150, 170, dot), (200, 170, (255, 255, 255)), (50, 120, dot), (115, 120, (0, 0, 0))])
    # The custom CFF encoding maps A to an owned thin-stem glyph called C65.
    # The PDF changes C only, so the native A mapping must remain in use.
    objects = base(b"BT /F1 20 Tf 20 100 Td (AC) Tj ET")
    objects[3] = (b"<< /Type /Font /Subtype /Type1 /BaseFont /FastPDFThinStem /FirstChar 65 /LastChar 67 "
                  b"/Widths [600 600 600] /Encoding 7 0 R /FontDescriptor << /Type /FontDescriptor "
                  b"/FontName /FastPDFThinStem /Flags 4 /FontBBox [0 0 600 700] /Ascent 700 /Descent 0 "
                  b"/CapHeight 700 /StemV 40 /ItalicAngle 0 /FontFile3 6 0 R >> >>")
    objects.append(stream((Path(__file__).parent / "data" / "thin-stem.cff").read_bytes(), b"/Subtype /Type1C"))
    objects.append(b"<< /Differences [67 /A] >>")
    add("cff_native_differences", pdf(objects), pixels=[(22, 110, (51, 51, 51)),
                                                       (23, 110, (255, 255, 255)), (35, 110, (0, 0, 0))], tolerance=16)
    cmyk_jpeg = (Path(__file__).parent / "data" / "cmyk-panels.jpg").read_bytes()
    # The JPEG has four constant CMYK panels. Its original JPEG samples have
    # opposite polarity from WIC's normalized CMYK output.
    for inverse in (False, True):
        objects = base(b"q 256 0 0 64 20 100 cm /I Do Q", resources=b"/XObject << /I 6 0 R >>")
        attributes = (b"/Type /XObject /Subtype /Image /Width 64 /Height 16 "
                      b"/BitsPerComponent 8 /ColorSpace /DeviceCMYK /Filter /DCTDecode")
        if inverse:
            attributes += b" /Decode [1 0 1 0 1 0 1 0]"
        objects.append(stream(cmyk_jpeg, attributes))
        colors = [(255, 255, 255), (0, 255, 255), (255, 0, 0), (0, 0, 0)] if inverse else [(0, 0, 0)] * 4
        add(f"jpeg_cmyk_{inverse}", pdf(objects), pixels=[(52 + 64*i, 90, c) for i, c in enumerate(colors)])

    gray_jpeg = (Path(__file__).parent / "data" / "gray-tints.jpg").read_bytes()
    for inverse in [False, True]:
        objects = base(b"q 160 0 0 80 20 20 cm /Im Do Q", b"/XObject << /Im 6 0 R >>")
        attributes = (b"/Type /XObject /Subtype /Image /Width 32 /Height 16 /BitsPerComponent 8 "
                      b"/Filter /DCTDecode /ColorSpace [/Separation /Spot /DeviceRGB "
                      b"<< /FunctionType 2 /Domain [0 1] /C0 [1 1 1] /C1 [0 0 1] /N 1 >>]")
        if inverse:
            attributes += b" /Decode [1 0]"
        objects.append(stream(gray_jpeg, attributes))
        left, right = ((0, 0, 255), (255, 255, 255)) if inverse else ((255, 255, 255), (0, 0, 255))
        add(f"jpeg_spot_{inverse}", pdf(objects), pixels=[(50, 150, left), (150, 150, right)])
    jpx_root = Path(__file__).parent / "data"
    jpx = (jpx_root / "L-lossless.jp2").read_bytes()
    raw = (jpx_root / "L-lossless.raw").read_bytes()
    def jp2_box(kind, data):
        return struct.pack(">I4s", len(data) + 8, kind) + data
    # The PDF palette takes precedence over the embedded JP2 palette.
    embedded_palette = jp2_box(b"pclr", struct.pack(">HB", 256, 3) + b"\x07\x07\x07" + b"\x00\x00\xff" * 256)
    channel_map = jp2_box(b"cmap", b"".join(struct.pack(">HBB", 0, 1, i) for i in range(3)))
    position, mapped_jpx = 0, bytearray()
    while position < len(jpx):
        length, kind = struct.unpack_from(">I4s", jpx, position)
        body = jpx[position + 8:position + length]
        if kind == b"jp2h":
            body += embedded_palette + channel_map
        mapped_jpx += jp2_box(kind, body)
        position += length
    palette = bytes(c for i in range(256) for c in (i, 255 - i, 0))
    for label, data in [("plain", jpx), ("mapped", mapped_jpx)]:
        objects = base(b"q 37 0 0 29 0 0 cm /Im Do Q", b"/XObject << /Im 6 0 R >>")
        objects[2] = objects[2].replace(b"300 220", b"37 29")
        objects.append(stream(data, b"/Type /XObject /Subtype /Image /Width 37 /Height 29 /Interpolate false "
                              b"/Filter /JPXDecode /ColorSpace [/Indexed /DeviceRGB 255 <" + palette.hex().encode() + b">]"))
        pixels = [(x, y, (raw[y * 37 + x], 255 - raw[y * 37 + x], 0)) for x, y in [(5, 5), (20, 10), (30, 20)]]
        add(f"jpx_indexed_{label}", pdf(objects), size=(37, 29), pixels=pixels)
    rng = random.Random(181)
    noise = b"%" + bytes(rng.randrange(33, 126) for _ in range(24000)) + b"\n"
    for level in [0, 1, 6, 9]:
        objects = base()
        objects[4] = stream(zlib.compress(noise + CONTENT, level), b"/Filter /FlateDecode")
        add(f"flate{level}", pdf(objects), text="FastPDF hello world")
    for name, data, filt in [
        ("ascii85", base64.a85encode(zlib.compress(CONTENT)) + b"~>", b"[/ASCII85Decode /FlateDecode]"),
        ("asciihex", zlib.compress(CONTENT).hex().encode() + b">", b"[/ASCIIHexDecode /FlateDecode]"),
        ("runlength", bytes([len(CONTENT)-1]) + CONTENT + b"\x80", b"/RunLengthDecode"),
    ]:
        objects = base()
        objects[4] = stream(data, b"/Filter " + filt)
        add(name, pdf(objects), text="FastPDF hello world")
    for early in [0, 1]:
        objects = base()
        objects[4] = stream(lzw_encode(noise + CONTENT, early), f"/Filter /LZWDecode /DecodeParms << /EarlyChange {early} >>".encode())
        add(f"lzw{early}", pdf(objects), text="FastPDF hello world")
    add("object_stream", object_stream_pdf(), text="FastPDF hello world")
    original=object_stream_pdf(True)
    previous=int(original.rsplit(b'startxref\n',1)[1].splitlines()[0])
    replacement=base(extra_page=b'/Rotate 90')[2]
    updated=original+b'3 0 obj\n'+replacement+b'\nendobj\n'
    position=len(updated)
    updated+=f'xref\n3 1\n{len(original):010d} 00000 n \ntrailer\n<< /Size 8 /Root 1 0 R /Prev {previous} >>\nstartxref\n{position}\n%%EOF\n'.encode()
    add('hybrid_incremental',updated,text='FastPDF hello world',size=(220,300))
    add("repair", pdf(base()).replace(b"startxref\n", b"startxref\n99999999%"), text="FastPDF hello world", repaired=True)
    original = pdf(base())
    previous = int(original.rsplit(b"startxref\n", 1)[1].splitlines()[0])
    updated = original + b"5 0 obj\n" + stream(b"0 0 1 rg 20 20 100 80 re f\n" + TEXT) + b"\nendobj\n"
    xref = len(updated)
    updated += f"xref\n5 1\n{len(original):010d} 00000 n \ntrailer\n<< /Size 6 /Root 1 0 R /Prev {previous} >>\nstartxref\n{xref}\n%%EOF\n".encode()
    add("incremental", updated, text="FastPDF hello world", pixel=(50, 150, (0, 0, 255)))
    for rotation in [90, 180, 270]:
        add(f"rotate{rotation}", pdf(base(extra_page=f"/Rotate {rotation}".encode())), size=(220, 300) if rotation % 180 else (300, 220))
    add("crop", pdf(base(extra_page=b"/CropBox [10 20 200 190] /UserUnit 2")), size=(380, 340))
    for revision in range(2, 7):
        add(f"encrypted_r{revision}", encrypted_pdf(revision), text="FastPDF hello world", password="secret", owner="owner")
    add("encrypted_empty", encrypted_pdf(6, b""), text="FastPDF hello world")
    samples = b"\xff\0\0\0\0\xff"
    inline = (b"q 40 0 0 40 20 100 cm BI /W 2 /H 1 /BPC 8 /CS /RGB ID " + samples + b" EI Q "
              b"q 40 0 0 40 80 100 cm BI /W 2 /H 1 /BPC 8 /CS /RGB /F /FlateDecode ID " +
              zlib.compress(samples) + b" EI Q")
    for revision in (2, 4, 6):
        add(f"encrypted_inline_r{revision}", encrypted_pdf(revision, b"", content=inline),
            pixels=[(25, 100, (255, 0, 0)), (55, 100, (0, 0, 255)),
                    (85, 100, (255, 0, 0)), (115, 100, (0, 0, 255))])
    add("encrypted_unicode_r6",encrypted_pdf(6,b'IX'),text="FastPDF hello world",password='\u2168')
    add("encrypted_unicode_r3",encrypted_pdf(3,b'caf\xe9'),text="FastPDF hello world",password='caf\u00e9')
    add('jbig2_shared_symbols',jbig2_huffman_fixture(),pixel=(38,114,(0,0,0)))
    add('jbig2_streamed_rows',jbig2_streamed_fixture(),pixel=(40,88,(255,255,255)))
    add('jbig2_random_access',jbig2_streamed_fixture(True),pixel=(40,120,(0,0,0)))
    for name in ['RGB-lossless','RGBA-lossless']:
        data=(Path(__file__).parent/'data'/(name+'.jp2')).read_bytes()
        objects=base(b'q 74 0 0 58 20 20 cm /Im Do Q',b'/XObject << /Im 6 0 R >>')
        objects.append(stream(data,b'/Type /XObject /Subtype /Image /Width 37 /Height 29 /Filter /JPXDecode'+(b' /SMaskInData 1' if name.startswith('RGBA') else b'')))
        add('jpx_'+name,pdf(objects))
    # Raw RGB, indexed colors, PNG prediction, soft image masks, and inline images.
    raw = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])
    image_attrs = b"/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Interpolate false"
    for name, content in [("image_rgb", raw), ("image_flate", raw)]:
        objects = base(b"q 100 0 0 100 20 20 cm /Im Do Q", b"/XObject << /Im 6 0 R >>")
        objects.append(stream(content, image_attrs, name.endswith("flate")))
        add(name, pdf(objects), pixel=(40, 120, (255, 0, 0)))
    objects = base(b"q 100 0 0 100 20 20 cm /Im Do Q", b"/XObject << /Im 6 0 R >>")
    objects.append(stream(zlib.compress(b"\0"+raw[:6]+b"\0"+raw[6:]), image_attrs+b" /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 3 /Columns 2 >>"))
    add("image_predictor", pdf(objects), pixel=(40, 120, (255, 0, 0)))
    objects = base(b"q 100 0 0 100 20 20 cm /Im Do Q", b"/XObject << /Im 6 0 R >>")
    objects.append(stream(b"\x00\x01\x02\x03", b"/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace [/Indexed /DeviceRGB 3 <ff000000ff000000ffffffff>] /BitsPerComponent 8 /Interpolate false"))
    add("image_indexed", pdf(objects), pixel=(40, 120, (255, 0, 0)))
    add("inline_raw", pdf(base(b"q 100 0 0 100 20 20 cm BI /W 2 /H 2 /CS /RGB /BPC 8 /I false ID " + raw + b" EI Q")), pixel=(40, 120, (255, 0, 0)))
    add("inline_flate", pdf(base(b"q 100 0 0 100 20 20 cm BI /W 2 /H 2 /CS /RGB /BPC 8 /F /Fl /I false ID " + zlib.compress(raw) + b" EI Q")), pixel=(40, 120, (255, 0, 0)))
    objects = base(b"q 100 0 0 100 20 20 cm /Im Do Q", b"/XObject << /Im 6 0 R >>")
    objects += [stream(raw, image_attrs+b" /SMask 7 0 R"), stream(b"\x80"*4,b"/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 8")]
    add("image_smask", pdf(objects), pixel=(40, 120, (255, 127, 127)), tolerance=3)
    for name, ext, expected in [("alpha", b"/ca 0.5", (255,128,128)), ("multiply",b"/BM /Multiply",(0,0,0))]:
        content = (b"0 0 1 rg 20 20 100 100 re f " if name=="multiply" else b"") + b"/GS gs 1 0 0 rg 20 20 100 100 re f"
        objects = base(content,b"/ExtGState << /GS 6 0 R >>") + [b"<< "+ext+b" >>"]
        add(name,pdf(objects),pixel=(50,150,expected),tolerance=3)
    # A non-isolated group must multiply its red mark with the real blue
    # backdrop before applying the group's half opacity: the result is navy.
    objects=base(b'0 0 1 rg 20 20 100 100 re f /GA gs /Fm Do',b'/ExtGState << /GA 6 0 R >> /XObject << /Fm 7 0 R >>')
    objects += [b'<< /ca 0.5 >>',stream(b'/BM gs 1 0 0 rg 20 20 100 100 re f',b'/Type /XObject /Subtype /Form /BBox [20 20 120 120] /Group << /S /Transparency /I false >> /Resources << /ExtGState << /BM << /BM /Multiply >> >> >>')]
    add('group_backdrop',pdf(objects),pixel=(50,150,(0,0,128)),tolerance=2)
    nested=base(b'/Outer Do',b'/XObject << /Outer 8 0 R >>')+objects[5:]
    nested.append(stream(b'/GA gs 0 0 1 rg 20 20 100 100 re f /Fm Do',b'/Type /XObject /Subtype /Form /BBox [20 20 120 120] /Group << /S /Transparency /I true /CS /DeviceRGB >> /Resources << /ExtGState << /GA 6 0 R >> /XObject << /Fm 7 0 R >> >>'))
    add('group_translucent_backdrop',pdf(nested),pixel=(50,150,(128,64,128)),tolerance=2)
    objects[5]=b'<< /ca 0.5 /SMask << /S /Luminosity /G 8 0 R >> >>'
    objects.append(stream(b'0.5 g 20 20 100 100 re f',b'/Type /XObject /Subtype /Form /BBox [20 20 120 120] /Group << /S /Transparency /I true /CS /DeviceGray >>'))
    add('group_masked_backdrop',pdf(objects),pixel=(50,150,(0,0,191)),tolerance=2)
    objects=base(b"/GS gs 1 0 0 rg 20 20 100 100 re f",b"/ExtGState << /GS 6 0 R >>")
    objects += [b"<< /SMask << /S /Luminosity /G 7 0 R >> >>",stream(b"0.5 g 0 0 300 220 re f",b"/Type /XObject /Subtype /Form /BBox [0 0 300 220] /Group << /S /Transparency /CS /DeviceGray /I true >>")]
    add("graphics_smask",pdf(objects),pixel=(50,150,(255,128,128)),tolerance=3)
    objects=base(b"/Pattern cs /P scn 20 20 100 100 re f",b"/Pattern << /P 6 0 R >>")
    objects += [stream(b"0 0 1 rg 0 0 5 10 re f",b"/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 10 10] /XStep 10 /YStep 10 /Resources << >>")]
    add("pattern",pdf(objects),pixel=(22,150,(0,0,255)))
    objects=base(b"20 20 100 100 re W n /Sh sh",b"/Shading << /Sh 6 0 R >>")
    objects += [b'<< /ShadingType 1 /ColorSpace /DeviceRGB /Domain [0 1 0 1] /Matrix [100 0 0 100 20 20] /Function 7 0 R >>',stream(b'{ pop dup 0 exch 1 exch sub }',b'/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1]')]
    add('function_shading',pdf(objects),pixel=(70,150,(129,0,126)),tolerance=4)
    objects=base(b"20 20 100 100 re W n /Sh sh",b"/Shading << /Sh 6 0 R >>")
    objects += [b"<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [20 20 120 20] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> /Extend [true true] >>"]
    add("axial",pdf(objects),pixel=(70,150,(126,0,129)),tolerance=4)
    objects[4] = stream(b"q 0 0 0 0 50 50 cm /Sh sh Q 0 1 0 rg 20 20 100 100 re f")
    add("collapsed_shading", pdf(objects), pixels=[(70, 150, (0, 255, 0)), (150, 150, (255, 255, 255))])
    objects = base(b"q 20 20 100 40 re W n /A sh Q q 20 120 100 40 re W n /B sh Q",
                   b"/Shading << /A 6 0 R /B 7 0 R >>")
    objects += [b"<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [20 20 120 20] /Domain [0 .5] /Function 8 0 R >>",
                b"<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [20 120 120 120] /Domain [.5 1] /Function 8 0 R >>",
                b"<< /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >>"]
    add("shared_gradient_domains", pdf(objects), pixels=[(70, 180, (64, 64, 64)), (70, 80, (192, 192, 192))], tolerance=2)
    objects=base(b"20 20 100 100 re W n /Sh sh",b"/Shading << /Sh 6 0 R >>")
    objects += [b"<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [70 70 0 70 70 50] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> /Extend [true true] >>"]
    add("radial",pdf(objects),pixel=(70,150,(251,0,4)),tolerance=5)
    objects=base(b"20 20 100 100 re W n /Sh sh",b"/Shading << /Sh 6 0 R >>")
    objects += [b"<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [20 20 120 20] /Function 7 0 R /Extend [true true] >>",stream(b"{ dup 0 exch 1 exch sub }",b"/FunctionType 4 /Domain [0 1] /Range [0 1 0 1 0 1]")]
    add("calculator",pdf(objects),pixel=(70,150,(129,0,126)),tolerance=4)
    for shading in [6,7]:
        points=[(20,20),(20,53),(20,87),(20,120),(53,120),(87,120),(120,120),(120,87),(120,53),(120,20),(87,20),(53,20)]
        if shading==7: points += [(53,53),(53,87),(87,87),(87,53)]
        data=b"\0"+bytes(n for p in points for n in p)+bytes([255,0,0,255,0,0,0,0,255,0,0,255])
        objects=base(b"/Sh sh",b"/Shading << /Sh 6 0 R >>")
        objects += [stream(data,f"/ShadingType {shading} /ColorSpace /DeviceRGB /BitsPerCoordinate 8 /BitsPerComponent 8 /BitsPerFlag 8 /Decode [0 255 0 255 0 1 0 1 0 1]".encode())]
        add(f"patch{shading}",pdf(objects),pixel=(70,150,(126,0,129)),tolerance=4)
    for shading in [4,5]:
        points=[(20,20),(120,20),(20,120)] if shading==4 else [(20,20),(120,20),(20,120),(120,120)]
        data=b"".join((b"\0" if shading==4 else b"")+bytes(p)+bytes([255,0,0]) for p in points)
        objects=base(b"/Sh sh",b"/Shading << /Sh 6 0 R >>")
        objects += [stream(data,f"/ShadingType {shading} /ColorSpace /DeviceRGB /BitsPerCoordinate 8 /BitsPerComponent 8 /BitsPerFlag 8 /VerticesPerRow 2 /Decode [0 255 0 255 0 1 0 1 0 1]".encode())]
        add(f"mesh{shading}",pdf(objects),pixel=(50,150,(255,0,0)))
    add("type1",type1_fixture(),text="AAA",pixel=(50,150,(0,0,0)))
    objects=base(b"BT /F1 100 Tf 20 50 Td (AAA) Tj ET")
    objects[3]=b"<< /Type /Font /Subtype /Type3 /Name /F1 /FontBBox [0 0 600 700] /FontMatrix [.001 0 0 .001 0 0] /FirstChar 65 /LastChar 65 /Widths [600] /Encoding << /Differences [65 /A] >> /CharProcs << /A 6 0 R >> /Resources << >> >>"
    objects += [stream(b"600 0 d0 0 0 500 700 re f")]
    add("type3",pdf(objects),text="AAA",pixel=(50,150,(0,0,0)))
    # Type 3 widths use FontMatrix, including across separate text operations.
    objects=base(b"BT /F1 40 Tf 20 160 Td (AAA) Tj 0 -60 Td (A) Tj (A) Tj (A) Tj "
                 b"0 -60 Td 3 Tc 5 Tw 80 Tz [(A) -100 ( ) 100 (A)] TJ (A) Tj ET")
    widths=b"25 " + b"0 " * 32 + b"50"
    objects[3]=(b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 40 60] "
                b"/FontMatrix [.01204 0 0 .01204 0 0] /FirstChar 32 /LastChar 65 "
                b"/Widths [" + widths + b"] /Encoding << /Differences [32 /space 65 /A] >> "
                b"/CharProcs << /A 6 0 R /space 7 0 R >> /Resources << >> >>")
    objects += [stream(b"50 0 d0 0 0 40 60 re f"),stream(b"25 0 d0")]
    add("type3_font_matrix",pdf(objects),text="AAA",pixels=[
        (30,45,(0,0,0)),(41,45,(255,255,255)),(50,45,(0,0,0)),(75,45,(0,0,0)),
        (30,105,(0,0,0)),(41,105,(255,255,255)),(50,105,(0,0,0)),(75,105,(0,0,0)),
        (25,170,(0,0,0)),(41,170,(255,255,255)),(62,170,(0,0,0)),
        (76,170,(255,255,255)),(84,170,(0,0,0))])
    objects=base(extra_page=b"/Annots [6 0 R]")
    objects += [b"<< /Type /Annot /Subtype /Square /Rect [140 20 200 100] /AP << /N 7 0 R >> >>",stream(b"0 1 0 rg 0 0 10 10 re f",b"/Type /XObject /Subtype /Form /BBox [0 0 10 10]")]
    add("annotation",pdf(objects),pixel=(160,150,(0,255,0)))
    objects=base(extra_page=b'/Annots [6 0 R]')
    objects += [b'<< /Type /Annot /Subtype /Widget /FT /Tx /Rect [20 110 250 160] /V (Owned form appearance) /DA (/Helv 18 Tf 0 g) /MK << /BG [0.8 0.9 1] /BC [0 0 0] >> >>']
    add('widget_without_appearance',pdf(objects),text='Owned form appearance',pixel=(240,100,(204,230,255)))
    objects=base(b"q 20 20 20 20 re W n 1 0 0 rg 0 0 300 220 re f Q 0 0 1 rg 60 20 20 20 re f")
    add("clip_restore",pdf(objects),pixel=(70,190,(0,0,255)))
    objects=base(extra_page=b"/Annots [6 0 R]",extra_catalog=b"/Outlines 7 0 R")
    objects += [b"<< /Type /Annot /Subtype /Link /Rect [20 170 200 200] /A << /S /URI /URI (https://example.com) >> >>", b"<< /Type /Outlines /First 8 0 R /Last 8 0 R /Count 1 >>", b"<< /Title (Start) /Parent 7 0 R /Dest [3 0 R /Fit] >>"]
    add("links_outline",pdf(objects),text="FastPDF hello world")
    # Explicit rejection of cyclic page trees and oversized image allocation.
    objects=base()
    objects[1]=b"<< /Type /Pages /Kids [2 0 R] /Count 1 >>"
    add("invalid_cycle",pdf(objects),error=True)
    add("invalid_header",b"This is not a PDF",error=True)
    return cases


if __name__ == "__main__":
    import json,sys
    destination = Path(sys.argv[1])
    cases=generate(destination)
    (destination / "cases.json").write_text(json.dumps(cases,indent=2),encoding="utf-8")
    print(f"Generated {len(cases)} PDFs in {destination}")
