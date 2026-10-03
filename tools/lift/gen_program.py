#!/usr/bin/env python3
"""Generate the translated Sub-CPU program (SP + SUBCODE) as C++ from the user's disc blobs.
Usage: tools/lift/gen_program.py <outdir> [code dir default extracted/code]
Writes <outdir>/sub_translated.cpp. Output is disc-derived: never commit or distribute it."""
import os, sys, zlib, re
sys.path.insert(0, os.path.dirname(__file__))
from m68k_decode import decode, hx
from emit import flow_code

PROGRAMS = {
    'sub': dict(regions=[('scd_sub_sp.bin', 0x6000), ('scd_subcode.bin', 0xD400)],
                entries=[0x602A, 0x602E, 0x60A8, 0x60BE, 0x629C], vectors=True, mask=0xFFFFFF, lo_code=0x6000),
    'main': dict(regions=[('scd_main_ip.bin', 0xFF0000)],
                 entries=[0xFF0000, 0xFF0584, 0xFF06C8, 0xFF08E2], vectors=False, mask=0xFFFFFF, lo_code=0xFF0000),
}



def static_target(ins):
    if ins.flow in (('jump_dyn',), ('call_dyn',)) and len(ins.code) >= 2:
        m = re.fullmatch(r'uint32_t t\d+ = (0x[0-9A-F]+);', ins.code[0])
        if m: return int(m.group(1), 16)
    return None

def main():
    outdir = sys.argv[1]
    codedir = sys.argv[2] if len(sys.argv) > 2 else 'extracted/code'
    prog = sys.argv[3] if len(sys.argv) > 3 else 'sub'
    cfg = PROGRAMS[prog]
    REGIONS, EXTRA_ENTRIES = cfg['regions'], cfg['entries']
    mem = {}
    spans = []
    for f, base in REGIONS:
        d = open(os.path.join(codedir, f), 'rb').read()
        for i, b in enumerate(d): mem[base + i] = b
        spans.append((base, base + len(d), zlib.crc32(d) & 0xFFFFFFFF, f))
    r8 = lambda a: mem.get(a, 0)
    r16 = lambda a: (r8(a) << 8) | r8(a + 1)
    r32 = lambda a: (r16(a) << 16) | r16(a + 2)
    in_code = lambda a: a % 2 == 0 and any(lo <= a < hi for lo, hi, _, _ in spans)  # (spans hold 24-bit addresses)

    entries = set(EXTRA_ENTRIES)
    if cfg['vectors']:
        for a in range(0xD404, 0xD48A, 6):          # SUBCODE vector table: jmp abs.l
            if r16(a) == 0x4EF9: entries.add(r32(a + 2))
    if prog == 'main':
        for lo, hi, _, _ in spans:
            for a in range(lo, hi - 4, 2):
                v = r32(a)
                if (v >> 16) == 0xFFFF and in_code(v & 0xFFFFFF): entries.add(v & 0xFFFFFF)   # RAM vector tables (dc.l $FFFFxxxx)
                if r16(a) == 0x31FC:
                    w = r16(a + 2)
                    if 0x400 <= w < 0x6600 and in_code(0xFF0000 | w): entries.add(0xFF0000 | w)   # H-INT vector words
    # code pointers stored as immediates / lea / pea
    for lo, hi, _, _ in spans:
        for a in range(lo, hi - 6, 2):
            w = r16(a)
            if w in (0x21FC, 0x2D7C, 0x2F3C, 0x203C, 0x227C, 0x207C, 0x41F9, 0x43F9, 0x45F9, 0x47F9, 0x49F9, 0x4BF9, 0x4DF9, 0x4879, 0x4EF9, 0x4EB9):
                v = r32(a + 2) & 0xFFFFFF
                if in_code(v) and (v >= cfg['lo_code']): entries.add(v)

    insns = {}
    work = sorted(entries)
    tables = 0
    def table_targets(ins):
        """jmp/jsr d(pc,dn) tables: [offset words] or [long pointers] (best effort)."""
        nonlocal tables
        t = []
        sym = getattr(ins, 'static', None)
        return t
    while work:
        pc = work.pop()
        while pc not in insns and in_code(pc):
            ins = decode(r16, pc)
            insns[pc] = ins
            f = ins.flow
            nxt = pc + ins.size
            if isinstance(f, tuple) and f[0] in ('goto', 'cond', 'call'):
                work.append(f[-1])
            st = static_target(ins)
            if st is not None: work.append(st)
            if f == ('self',):
                # dbcc target is encoded in code as constant: re-derive
                m = re.search(r'npc = (0x[0-9A-F]+); \}', ' '.join(ins.code))
                if m: work.append(int(m.group(1), 16))
            if f in ('next', ('self',)) or (isinstance(f, tuple) and f[0] in ('cond', 'call', 'call_dyn')):
                pc = nxt; continue
            break
    # pc-relative jump tables: scan decoded insns for 'jmp d(pc,dN)' preceded by move.w d(pc,dN),dN (offset table)
    changed = True
    while changed:
        changed = False
        for pc in sorted(insns):
            ins = insns[pc]
            if ins.flow == ('jump_dyn',) and r16(pc) == 0x4EFB:
                ext = r16(pc + 2)
                base = pc + 2 + ((ext & 0xFF) - 256 if ext & 0x80 else ext & 0xFF)
                first = r16(base)
                n = first // 2 if 0 < first < 0x200 else 0
                for i in range(n):
                    tgt = (base + (r16(base + 2 * i) - 0x10000 if r16(base + 2 * i) & 0x8000 else r16(base + 2 * i))) & 0xFFFFFF
                    if in_code(tgt) and tgt not in insns:
                        work.append(tgt); changed = True
            if ins.flow in (('jump_dyn',), ('call_dyn',)) and r16(pc - 4) == 0x207B if False else False:
                pass
        # long-pointer tables used by movea.l d(pc,dN),a0 ; jmp (a0): scan for 4ef0/4ed0 preceded by 207b
        for pc in sorted(insns):
            if r16(pc) == 0x207B and r16(pc + 4) in (0x4ED0, 0x4E90):
                ext = r16(pc + 2); base = pc + 2 + ((ext & 0xFF) - 256 if ext & 0x80 else ext & 0xFF)
                for i in range(0, 256):
                    v = r32(base + 4 * i)
                    if not in_code(v & 0xFFFFFF) or (v >> 24) not in (0, 0xFF) and v > 0xFFFFFF and False: break
                    tgt = v & 0xFFFFFF
                    if tgt not in insns and in_code(tgt): work.append(tgt); changed = True
                    if tgt in (0,) : break
        while work:
            pc = work.pop()
            while pc not in insns and in_code(pc):
                ins = decode(r16, pc); insns[pc] = ins; f = ins.flow; nxt = pc + ins.size
                if isinstance(f, tuple) and f[0] in ('goto', 'cond', 'call'): work.append(f[-1])
                st = static_target(ins)
                if st is not None: work.append(st)
                if f == ('self',):
                    m = re.search(r'npc = (0x[0-9A-F]+); \}', ' '.join(ins.code))
                    if m: work.append(int(m.group(1), 16))
                if f in ('next', ('self',)) or (isinstance(f, tuple) and f[0] in ('cond', 'call', 'call_dyn')):
                    pc = nxt; continue
                break
            changed = True

    order = sorted(insns)
    os.makedirs(outdir, exist_ok=True)
    out = ['// GENERATED from the user\'s disc by tools/lift/gen_program.py. Do not commit or distribute.',
           '#include "cpu68k.h"', 'extern "C" { extern unsigned char m68ki_cycles[][0x10000]; }', 'namespace lift {', '']
    out.append('bool %s_has(uint32_t pc)' % prog + ' {\n    switch (pc) {')
    for pc in order: out.append('        case 0x%X:' % pc)
    out.append('            return true;\n        default: return false;\n    }\n}\n')
    out.append('extern const Span k%sSpans[] = {' % prog.capitalize())
    for lo, hi, crc, f in spans: out.append('    {0x%X, 0x%X, 0x%08X},  // %s' % (lo, hi, crc, f))
    out.append('};\nextern const int k%sSpanCount = %d;\n' % (prog.capitalize(), len(spans)))
    out.append('Exit %s_run(Cpu& c, uint32_t pc, int32_t& budget) {' % prog + '\n    uint32_t npc = 0, tgt = 0; (void)tgt;\n    for (;;) {\n        if (budget <= 0) return Exit{pc, Exit::Budget};\n        switch (pc) {')
    for i, pc in enumerate(order):
        ins = insns[pc]
        op = r16(pc)
        out.append('        case 0x%X: { // %s' % (pc, ''))
        out.append('            budget -= m68ki_cycles[0][0x%04X];' % op)
        if ins.code and ins.code[0].startswith('/* unsupported'):
            out.append('            return Exit{0x%X, Exit::Unsupported};\n        }' % pc); continue
        for line in ins.code + flow_code(ins): out.append('            ' + line)
        nxt = pc + ins.size
        if ins.flow == 'next' and nxt in insns and i + 1 < len(order) and order[i + 1] == nxt:
            out.append('            if (budget <= 0) return Exit{0x%X, Exit::Budget};' % nxt)
            out.append('        } // fall through\n        [[fallthrough]];')
            continue
        out.append('            pc = npc & 0xFFFFFF; continue;\n        }')
    out.append('        default: return Exit{pc, Exit::NotTranslated};\n        }\n    }\n}\n}  // namespace lift')
    open(os.path.join(outdir, '%s_translated.cpp' % prog), 'w').write('\n'.join(out) + '\n')
    print('translated %d instructions (%d entry points)' % (len(order), len(entries)))


main()
