"""Verify the real reader restores its page, rotation, and zoom after restart."""
from pathlib import Path
import struct
import subprocess
import sys
import zlib
from fixtures import pdf, stream


def blue_bounds(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    position = 8
    compressed = bytearray()
    while position < len(data):
        length = struct.unpack_from('>I', data, position)[0]
        kind = data[position + 4:position + 8]
        payload = data[position + 8:position + 8 + length]
        if kind == b'IHDR':
            width, height, depth, color = struct.unpack_from('>IIBB', payload)
            assert depth == 8 and color in (2, 6)
            channels = 3 if color == 2 else 4
        elif kind == b'IDAT':
            compressed += payload
        position += length + 12
    raw = zlib.decompress(compressed)
    stride = width * channels
    previous = bytearray(stride)
    points = []
    for y in range(height):
        offset = y * (stride + 1)
        mode = raw[offset]
        row = bytearray(raw[offset + 1:offset + 1 + stride])
        for i in range(stride):
            a = row[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if mode == 1:
                delta = a
            elif mode == 2:
                delta = b
            elif mode == 3:
                delta = (a + b) // 2
            elif mode == 4:
                p = a + b - c
                distances = [abs(p - a), abs(p - b), abs(p - c)]
                delta = [a, b, c][distances.index(min(distances))]
            else:
                assert mode == 0
                delta = 0
            row[i] = (row[i] + delta) & 255
        for x in range(width):
            r, g, b = row[x * channels:x * channels + 3]
            if r < 5 and g < 5 and b > 250:
                points.append((x, y))
        previous = row
    assert points, 'Restored page was not the blue second page'
    xs, ys = zip(*points)
    return max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def run(executable, root):
    root.mkdir(parents=True, exist_ok=True)
    document = root / 'restored document 文書.pdf'
    document.write_bytes(pdf([
        b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 5 0 R >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 6 0 R >>',
        stream(b'1 0 0 rg 0 0 200 100 re f'),
        stream(b'0 0 1 rg 0 0 200 100 re f'),
    ]))
    sizes = []
    for zoom in ('1', '1.5'):
        image = root / f'restored-{zoom}.png'
        subprocess.run([str(executable), '--restore-view', '1', '90', '2', zoom,
                        str(document), '--screenshot', str(image)], check=True, timeout=25)
        width, height = blue_bounds(image)
        assert abs(height / width - 2) < 0.06, (width, height)
        sizes.append(width)
    assert abs(sizes[1] / sizes[0] - 1.5) < 0.06, sizes
    print('Reader restored Unicode document path, second page, rotation, and custom zoom.')


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
