"""Original arithmetic streams for JBIG2 context and edge checks."""
import struct


def generic_stream(width, height, typical=False, custom=False):
    def segment(number, kind, data):
        return struct.pack('>IBBBI', number, kind, 0, 1, len(data)) + data

    page = struct.pack('>IIIIBH', width, height, 0, 0, 0, 0)
    # Fixed input bytes exercise both arithmetic outcomes. Expected pixels were
    # decoded independently with MuPDF, including narrow rows and row copying.
    payload = bytes((i * 73 + i // 11) % 251 for i in range(8192)) + b'\xff\xac'
    adaptive = bytes([2 if custom else 3, 255, 253, 255, 2, 254, 254, 254])
    region = struct.pack('>IIIIBB', width, height, 0, 0, 0, 8 if typical else 0)
    return segment(0, 48, page) + segment(1, 38, region + adaptive + payload)
