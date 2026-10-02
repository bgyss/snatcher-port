#!/usr/bin/env python3
"""Format MAME's Sega CD internal backup RAM (8 KB) so games can save.

A fresh MAME nvram file is all zeros. Snatcher then stops with "Return to control
screen, initialize at option screen". This writes the standard BRAM directory
footer: SEGA_CD_ROM / RAM_CARTRIDGE signature plus the free-block count. That is
the same image the BIOS produces when it formats internal BRAM.

Usage: tools/emu/format_bram.py [path] [--force]
       default path: work/mame/nvram/segacd/segacd_backupram
"""
import os
import struct
import sys

SIZE = 0x2000
FOOTER = (
    b"___________" + b"\x00\x00\x00\x00\x40"
    + b"\x00" * 16  # free-block counts, filled below
    + b"SEGA_CD_ROM\x00\x01\x00\x00\x00"
    + b"RAM_CARTRIDGE___"
)


def formatted():
    img = bytearray(SIZE)
    footer = bytearray(FOOTER)
    free = SIZE // 0x40 - 3
    footer[0x10:0x18] = struct.pack(">HHHH", free, free, free, free)
    img[SIZE - 0x40:] = footer
    return bytes(img)


def main():
    args = [a for a in sys.argv[1:] if a != "--force"]
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    path = args[0] if args else os.path.join(root, "work/mame/nvram/segacd/segacd_backupram")
    if os.path.exists(path) and "--force" not in sys.argv:
        cur = open(path, "rb").read()
        if any(cur):
            sys.exit(f"{path} already has data; use --force to wipe it")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(formatted())
    print(f"formatted {path}")


if __name__ == "__main__":
    main()
