#!/usr/bin/env python3
"""Extract the Sega CD 68000 code blobs for Ghidra into extracted/code/ (gitignored).

    scd_main_ip.bin   disc 0x200..0x6800  -> Main CPU 0xFF0000 (security block + IP)
    scd_sub_sp.bin    disc 0x6800..0x8000 -> Sub CPU 0x6000    (SP, module "MAIN A014")
    scd_subcode.bin   ISO /SUBCODE.BIN    -> Sub CPU 0xD400    (loaded by SP @0x61F2)

Usage: tools/extract_code.py <sega-cd .bin> [outdir]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from disc_inspect import RawTrack, iso_walk  # noqa: E402


def main():
    image = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "extracted/code"
    os.makedirs(out, exist_ok=True)
    t = RawTrack(image)
    boot = t.read(0, 0x8000)
    if not boot.startswith(b"SEGADISCSYSTEM"):
        sys.exit("not a Sega CD data track")

    blobs = {
        "scd_main_ip.bin": boot[0x200:0x6800],
        "scd_sub_sp.bin": boot[0x6800:0x8000],
    }
    blobs["ovl_909.bin"] = t.read(909, 49 * 2048)   # raw-area overlay loaded to Sub $28000 after the Konami logo (not an ISO file)
    pvd = t.sector(16)
    root = pvd[156:190]
    for name, lba, size in iso_walk(t, struct.unpack("<I", root[2:6])[0],
                                    struct.unpack("<I", root[10:14])[0]):
        if name == "/SUBCODE.BIN":
            blobs["scd_subcode.bin"] = t.read(lba, size)

    for name, data in blobs.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        print(f"{name:18} {len(data):6d} bytes")


if __name__ == "__main__":
    main()
