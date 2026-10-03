#!/usr/bin/env python3
"""Emit engine/tests/lift_cases.inc: one C++ function per distinct instruction found in the user's code blobs.
Usage: tools/lift/gen_tests.py out.inc [max_cases]   (reads extracted/code/*.bin; output is derived from the disc: do not commit)"""
import os, random, sys
sys.path.insert(0, os.path.dirname(__file__))
from m68k_decode import decode, hx
from emit import flow_code

BLOBS = [('extracted/code/scd_subcode.bin', 0xD400, 0x16300), ('extracted/code/scd_sub_sp.bin', 0x6000, 0x7164),
         ('extracted/code/scd_main_ip.bin', 0xFF0000, 0xFF5800)]


def main():
    out, cap = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    cases, seen = [], set()
    for path, base, end in BLOBS:
        data = open(path, 'rb').read()
        r16 = lambda a, d=data, b=base: (d[a - b] << 8 | d[a - b + 1]) if 0 <= a - b < len(d) - 1 else 0
        pc = base
        while pc < min(end, base + len(data) - 2):
            ins = decode(r16, pc)
            key = bytes(data[pc - base:pc - base + ins.size]) + (b'%d' % pc if ins.flow != 'next' else b'')
            if key not in seen and ins.flow != ('rte',) and data[pc - base:pc - base + 2][0] != 0x46 and 'unsupported' not in (ins.code[0] if ins.code else ''):
                seen.add(key); cases.append((ins, bytes(data[pc - base:pc - base + ins.size])))
            pc += ins.size
    random.Random(1).shuffle(cases); cases = cases[:cap]
    with open(out, 'w') as f:
        for i, (ins, raw) in enumerate(cases):
            f.write('static lift::Exit t_%d(lift::Cpu& c, uint32_t& npc) { uint32_t tgt = 0; (void)tgt;\n  ' % i)
            f.write('\n  '.join(ins.code + flow_code(ins)) + '\n  return lift::Exit{};\n}\n')
        f.write('struct Case { uint32_t pc; int size; uint8_t bytes[12]; lift::Exit (*fn)(lift::Cpu&, uint32_t&); };\nstatic const Case kCases[] = {\n')
        for i, (ins, raw) in enumerate(cases):
            f.write('  {%s, %d, {%s}, t_%d},\n' % (hx(ins.pc), len(raw), ','.join(str(b) for b in raw), i))
        f.write('};\n')
    print(len(cases), 'cases')


main()
