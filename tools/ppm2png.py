#!/usr/bin/env python3
"""PPM (P6) -> PNG with optional crop and integer upscale. No dependencies.
Usage: tools/ppm2png.py in.ppm out.png [scale] [x y w h]"""
import struct, sys, zlib

def read_ppm(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

w, h, px = read_ppm(sys.argv[1])
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
x0, y0, cw, ch = (map(int, sys.argv[4:8]) if len(sys.argv) > 7 else (0, 0, w, h))
rows = []
for y in range(y0, y0 + ch):
    row = b''.join(px[(y * w + x) * 3:(y * w + x) * 3 + 3] * scale for x in range(x0, x0 + cw))
    rows.extend([row] * scale)
write_png(sys.argv[2], cw * scale, ch * scale, b''.join(rows))
