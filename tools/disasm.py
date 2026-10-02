#!/usr/bin/env python3
"""Linear 68000 disassembly of a code blob (capstone). Output stays in extracted/.

Usage: tools/disasm.py <blob> <base-hex> [start-hex] [end-hex]
"""
import sys
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000, CS_MODE_BIG_ENDIAN


def main():
    blob = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 16)
    start = int(sys.argv[3], 16) if len(sys.argv) > 3 else base
    end = int(sys.argv[4], 16) if len(sys.argv) > 4 else base + len(blob)
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
    pc = start
    while pc < end:
        chunk = blob[pc - base : pc - base + 12]
        ins = next(md.disasm(chunk, pc), None)
        if ins is None:
            print(f"{pc:08x}: {blob[pc-base]:02x}{blob[pc-base+1]:02x}  .word")
            pc += 2
            continue
        print(f"{pc:08x}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
        pc += ins.size


main()
