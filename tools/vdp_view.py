#!/usr/bin/env python3
"""Render a VRAM dump (from scd_headless with SCD_VRAM=1) as a tile sheet PNG-able PPM.
Usage: tools/vdp_view.py vram.bin out.ppm [pal]"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
vram = d[:0x10000]
cram = struct.unpack('>64H', d[0x10000:0x10080])
pal = int(sys.argv[3]) if len(sys.argv) > 3 else 0
def rgb(i):
    c = cram[i]; return tuple(v * 36 for v in ((c >> 1) & 7, (c >> 5) & 7, (c >> 9) & 7))
cols = 32
rows = 0x10000 // 32 // cols
w, h = cols * 8, rows * 8
img = bytearray(w * h * 3)
for t in range(rows * cols):
    for y in range(8):
        for x in range(8):
            b = vram[t * 32 + y * 4 + x // 2]
            c = (b & 15) if x & 1 else (b >> 4)
            px = (c * 17, c * 17, c * 17) if pal < 0 else (rgb(pal * 16 + c) if c else (0, 0, 0))
            o = ((t // cols * 8 + y) * w + (t % cols) * 8 + x) * 3
            img[o:o+3] = bytes(px)
open(sys.argv[2], 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(img))
