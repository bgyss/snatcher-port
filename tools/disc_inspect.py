#!/usr/bin/env python3
"""Inspect a raw MODE1/2352 CD data track (Sega CD / PC Engine CD).

Prints the Sega CD system header (if present) and walks an ISO9660 file
system when one exists. Reads the disc image directly; writes nothing.

Usage:
    tools/disc_inspect.py <image.bin> [--track-offset SECTORS] [--extract DIR]
"""
import argparse
import os
import struct
import sys

RAW = 2352
DATA = 2048
HDR = 16  # 12 sync + 4 header bytes in a MODE1 raw sector


class RawTrack:
    def __init__(self, path, track_offset=0):
        self.f = open(path, "rb")
        self.base = track_offset

    def sector(self, lba):
        self.f.seek((self.base + lba) * RAW + HDR)
        return self.f.read(DATA)

    def read(self, lba, length):
        out = bytearray()
        n = (length + DATA - 1) // DATA
        for i in range(n):
            out += self.sector(lba + i)
        return bytes(out[:length])


def sega_header(sec0):
    if not sec0.startswith(b"SEGADISCSYSTEM"):
        return None
    f = {
        "disc_id": sec0[0x00:0x10],
        "volume_name": sec0[0x10:0x1B],
        "system_name": sec0[0x20:0x2B],
        "ip_offset": struct.unpack(">I", sec0[0x30:0x34])[0],
        "ip_size": struct.unpack(">I", sec0[0x34:0x38])[0],
        "sp_offset": struct.unpack(">I", sec0[0x40:0x44])[0],
        "sp_size": struct.unpack(">I", sec0[0x44:0x48])[0],
        "hw": sec0[0x100:0x110],
        "copyright": sec0[0x110:0x120],
        "title_domestic": sec0[0x120:0x150],
        "title_overseas": sec0[0x150:0x180],
        "serial": sec0[0x180:0x18E],
        "region": sec0[0x1F0:0x1F3],
    }
    return f


def iso_walk(t, lba, size, path=""):
    data = t.read(lba, size)
    pos = 0
    while pos < len(data):
        rlen = data[pos]
        if rlen == 0:
            pos = (pos // DATA + 1) * DATA
            continue
        rec = data[pos:pos + rlen]
        ext = struct.unpack("<I", rec[2:6])[0]
        sz = struct.unpack("<I", rec[10:14])[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += rlen
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace").split(";")[0]
        full = f"{path}/{name}"
        if flags & 2:
            yield from iso_walk(t, ext, sz, full)
        else:
            yield full, ext, sz


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--track-offset", type=int, default=0,
                    help="LBA where the data track starts inside the image")
    ap.add_argument("--extract", help="directory to extract ISO9660 files to")
    a = ap.parse_args()

    t = RawTrack(a.image, a.track_offset)
    sec0 = t.sector(0)
    h = sega_header(sec0)
    if h:
        print("== Sega CD system header ==")
        for k, v in h.items():
            print(f"  {k:15} {v!r}" if isinstance(v, bytes) else f"  {k:15} 0x{v:X}")
    else:
        print("== no Sega CD header; first 64 bytes ==")
        print(" ", sec0[:64])

    pvd = t.sector(16)
    if pvd[1:6] != b"CD001":
        print("== no ISO9660 PVD at sector 16 ==")
        return
    print("== ISO9660 ==")
    print("  volume id:", pvd[40:72].decode("ascii", "replace").strip())
    print("  volume blocks:", struct.unpack("<I", pvd[80:84])[0])
    root = pvd[156:190]
    rlba = struct.unpack("<I", root[2:6])[0]
    rsz = struct.unpack("<I", root[10:14])[0]
    files = list(iso_walk(t, rlba, rsz))
    total = 0
    for name, lba, sz in sorted(files, key=lambda x: x[1]):
        print(f"  {lba:7d} {sz:10d}  {name}")
        total += sz
    print(f"  {len(files)} files, {total} bytes")

    if a.extract:
        for name, lba, sz in files:
            out = os.path.join(a.extract, name.lstrip("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as o:
                o.write(t.read(lba, sz))
        print(f"  extracted to {a.extract}", file=sys.stderr)


if __name__ == "__main__":
    main()
