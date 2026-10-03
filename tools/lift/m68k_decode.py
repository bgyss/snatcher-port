"""68000 instruction decoder and C++ emitter for the build-time translator (see docs/ADR-002).

decode(read16, pc) -> Insn with .size (bytes), .code (list of C++ statements), .flow:
  'next'            falls through
  ('goto', addr)    unconditional branch (bra)
  ('cond', cc, addr) conditional branch; fall-through otherwise
  ('call', addr)    bsr/jsr with static target (pushes pc+size)
  ('jump_dyn',)     computed jump (target in C++ variable `tgt`)
  ('call_dyn',)     computed call
  ('ret',) ('rte',) ('stop',)
Generated statements use the runtime in engine/src/lift/cpu68k.h (variable `c` is lift::Cpu&, `B` the bus).
"""
from dataclasses import dataclass, field

SZ = {0: 1, 1: 2, 2: 4}


@dataclass
class Insn:
    pc: int
    size: int = 2
    text: str = ''
    code: list = field(default_factory=list)
    flow: object = 'next'


class Unsupported(Exception):
    pass


def s16(v): return v - 0x10000 if v & 0x8000 else v
def s8(v): return v - 0x100 if v & 0x80 else v
def hx(v): return '0x%X' % (v & 0xFFFFFFFF)


class Ctx:
    def __init__(self, read16, pc):
        self.r16 = read16
        self.pc = pc
        self.pos = pc + 2          # next extension word address
        self.tmp = 0

    def ext(self):
        v = self.r16(self.pos); self.pos += 2; return v

    def ext32(self):
        hi = self.ext(); lo = self.ext(); return (hi << 16) | lo

    def t(self):
        self.tmp += 1; return 't%d' % self.tmp


def index_expr(ctx, base, extw):
    """base + d8 + Xn.size*scale from a brief extension word."""
    xn = (extw >> 12) & 7
    reg = ('c.a[%d]' if extw & 0x8000 else 'c.d[%d]') % xn
    idx = 'int32_t(%s)' % reg if extw & 0x800 else 'int32_t(int16_t(%s))' % reg
    d8 = s8(extw & 0xFF)
    return '(%s + %s + (%d))' % (base, idx, d8)


def check(mode, reg, kinds, B=None):
    if B == 1 and mode == 1 and kinds in ('data', 'any'): raise Unsupported('byte op on An')
    ok = {'data': mode != 1, 'alt': mode not in (1,) and not (mode == 7 and reg >= 2), 'mem_ctl': mode in (2, 5, 6) or (mode == 7 and reg <= 3),
          'movem_st': mode in (2, 4, 5, 6) or (mode == 7 and reg <= 1), 'movem_ld': mode in (2, 3, 5, 6) or (mode == 7 and reg <= 3), 'any': True}
    if not ok[kinds]: raise Unsupported('illegal ea mode %d/%d for %s' % (mode, reg, kinds))


class Operand:
    """An effective-address operand of a given size. emit setup in code; then .read() / .write(v)."""
    def __init__(self, ctx, code, mode, reg, size, rmw=False):
        self.size, self.mode, self.reg, self.code = size, mode, reg, code
        self.ea = None
        self.imm = None
        self.kind = None
        B = size
        if mode == 0: self.kind = 'dn'
        elif mode == 1: self.kind = 'an'
        elif mode in (2, 3, 4, 5, 6) or (mode == 7 and reg in (0, 1, 2, 3)):
            e = ctx.t()
            if mode == 2: code.append('uint32_t %s = c.a[%d];' % (e, reg))
            elif mode == 3:
                step = 2 if (reg == 7 and B == 1) else B
                code.append('uint32_t %s = c.a[%d]; c.a[%d] += %d;' % (e, reg, reg, step))
            elif mode == 4:
                step = 2 if (reg == 7 and B == 1) else B
                code.append('c.a[%d] -= %d; uint32_t %s = c.a[%d];' % (reg, step, e, reg))
            elif mode == 5:
                code.append('uint32_t %s = c.a[%d] + (%d);' % (e, reg, s16(ctx.ext())))
            elif mode == 6:
                w = ctx.ext()
                code.append('uint32_t %s = %s;' % (e, index_expr(ctx, 'int32_t(c.a[%d])' % reg, w)))
            elif reg == 0: code.append('uint32_t %s = %s;' % (e, hx(s16(ctx.ext()))))
            elif reg == 1: code.append('uint32_t %s = %s;' % (e, hx(ctx.ext32())))
            elif reg == 2:
                base = ctx.pos; code.append('uint32_t %s = %s;' % (e, hx(base + s16(ctx.ext()))))
            else:
                base = ctx.pos; w = ctx.ext()
                code.append('uint32_t %s = %s;' % (e, index_expr(ctx, hx(base), w)))
            self.ea = e; self.kind = 'mem'
        elif mode == 7 and reg == 4:
            if B == 4: self.imm = hx(ctx.ext32())
            elif B == 2: self.imm = hx(ctx.ext())
            else: self.imm = hx(ctx.ext() & 0xFF)
            self.kind = 'imm'
        else:
            raise Unsupported('ea %d/%d' % (mode, reg))

    def need_mem(self):
        if self.kind != 'mem': raise Unsupported('needs memory ea')
        return self.ea

    def read(self):
        B = self.size
        if self.kind == 'dn': return '(c.d[%d] & 0x%X)' % (self.reg, {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[B])
        if self.kind == 'an': return '(c.a[%d] & 0x%X)' % (self.reg, {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[B])
        if self.kind == 'imm': return self.imm
        return 'lift::rd<%d>(*c.bus, %s)' % (B, self.ea)

    def write(self, v):
        B = self.size
        if self.kind == 'imm': raise Unsupported('write to immediate')
        if self.kind == 'dn': return 'c.d[%d] = lift::merge<%d>(c.d[%d], %s);' % (self.reg, B, self.reg, v)
        if self.kind == 'an': return 'c.a[%d] = lift::sx<%d>(%s);' % (self.reg, B, v) if B == 2 else 'c.a[%d] = %s;' % (self.reg, v)
        return 'lift::wr<%d>(*c.bus, %s, %s);' % (B, self.ea, v)


def add_cc(code, expr):
    code.append(expr)


def ccr_logic(code, B, val):
    code.append('lift::flags_logic<%d>(c, %s);' % (B, val))


def decode(read16, pc):
    ctx = Ctx(read16, pc)
    op = read16(pc)
    ins = Insn(pc)
    code = ins.code
    top = op >> 12
    try:
        _decode(ctx, op, top, ins, code)
    except Unsupported as e:
        ins.code = ['/* unsupported: %s */' % e, 'return lift::Exit{%s, lift::Exit::Unsupported};' % hx(pc)]
        ins.flow = ('stop',)
        ins.size = 2
        return ins
    ins.size = ctx.pos - pc
    return ins


def _decode(ctx, op, top, ins, code):
    r9 = (op >> 9) & 7
    mode = (op >> 3) & 7
    reg = op & 7
    pc = ctx.pc
    if top == 0:
        return _group0(ctx, op, ins, code)
    if top in (1, 2, 3):
        B = {1: 1, 3: 2, 2: 4}[top]
        dmode, dreg = (op >> 6) & 7, r9
        if B == 1 and mode == 1: raise Unsupported('move.b from An')
        src = Operand(ctx, code, mode, reg, B)
        v = ctx.t(); code.append('uint32_t %s = %s;' % (v, src.read()))
        if dmode == 1:
            if B == 1: raise Unsupported('movea.b')
            code.append('c.a[%d] = %s;' % (dreg, ('lift::sx<2>(%s)' % v) if B == 2 else v))
            return
        check(dmode, dreg, 'alt')
        dst = Operand(ctx, code, dmode, dreg, B)
        code.append(dst.write(v)); ccr_logic(code, B, v); return
    if top == 4:
        return _group4(ctx, op, ins, code)
    if top == 5:
        sz = (op >> 6) & 3
        if sz == 3:
            cc = (op >> 8) & 0xF
            if mode == 1:  # dbcc
                disp = s16(ctx.ext()); tgt = (pc + 2 + disp) & 0xFFFFFFFF
                code.append('npc = %s; if (!lift::cond(c, %d)) { int16_t cnt = int16_t(c.d[%d]) - 1; c.d[%d] = lift::merge<2>(c.d[%d], uint16_t(cnt)); if (cnt != -1) npc = %s; }' % (hx(pc + 4 + 0), cc, reg, reg, reg, hx(tgt)))
                ins.flow = ('self',); return
            check(mode, reg, 'alt')
            dst = Operand(ctx, code, mode, reg, 1)
            code.append(dst.write('(lift::cond(c, %d) ? 0xFF : 0x00)' % cc)); return
        B = SZ[sz]; q = r9 or 8
        if mode == 7 and reg >= 2: raise Unsupported('addq pc/imm')
        if mode == 1 and B == 1: raise Unsupported('addq.b An')
        dst = Operand(ctx, code, mode, reg, B)
        if dst.kind == 'an':
            code.append('c.a[%d] %s= %d;' % (reg, '-' if op & 0x100 else '+', q)); return
        s = ctx.t(); code.append('uint32_t %s = %s;' % (s, dst.read()))
        if op & 0x100: code.append(dst.write('lift::sub<%d>(c, %d, %s)' % (B, q, s)))
        else: code.append(dst.write('lift::add<%d>(c, %d, %s)' % (B, q, s)))
        return
    if top == 6:
        cc = (op >> 8) & 0xF; d8 = op & 0xFF
        base = pc + 2
        if d8 == 0: disp = s16(ctx.ext())
        elif d8 == 0xFF: raise Unsupported('bcc.l')
        else: disp = s8(d8)
        tgt = (base + disp) & 0xFFFFFFFF
        if cc == 0: ins.flow = ('goto', tgt)
        elif cc == 1: ins.flow = ('call', tgt)
        else: ins.flow = ('cond', cc, tgt)
        return
    if top == 7:
        if op & 0x100: raise Unsupported('moveq bit8')
        code.append('c.d[%d] = %s; lift::flags_logic<4>(c, c.d[%d]);' % (r9, hx(s8(op & 0xFF)), r9)); return
    if top in (8, 0xC):
        sz = (op >> 6) & 3
        if sz == 3:  # divu/divs / mulu/muls
            check(mode, reg, 'data')
            src = Operand(ctx, code, mode, reg, 2)
            s = ctx.t(); code.append('uint32_t %s = %s;' % (s, src.read()))
            if top == 8:
                signed = bool(op & 0x100)
                code.append('{ if (%s == 0) return lift::Exit{%s, lift::Exit::DivZero}; ' % (s, hx(pc)) +
                    ('int32_t dd = int32_t(c.d[%d]); int32_t dv = int16_t(%s); int64_t q = dd / dv; int32_t r = dd %% dv; ' % (r9, s) +
                     'if (q > 32767 || q < -32768) { c.v = true; } else { c.d[%d] = (uint32_t(uint16_t(r)) << 16) | uint16_t(q); c.n = q < 0; c.z = q == 0; c.v = false; c.c = false; } }' % r9
                     if signed else
                     'uint32_t dd = c.d[%d]; uint32_t dv = %s & 0xFFFF; uint32_t q = dd / dv; uint32_t r = dd %% dv; ' % (r9, s) +
                     'if (q > 0xFFFF) { c.v = true; } else { c.d[%d] = (r << 16) | q; c.n = q & 0x8000; c.z = q == 0; c.v = false; c.c = false; } }' % r9))
            else:
                if op & 0x100: code.append('c.d[%d] = uint32_t(int32_t(int16_t(c.d[%d])) * int32_t(int16_t(%s))); lift::flags_logic<4>(c, c.d[%d]);' % (r9, r9, s, r9))
                else: code.append('c.d[%d] = uint32_t(uint16_t(c.d[%d])) * uint32_t(uint16_t(%s)); lift::flags_logic<4>(c, c.d[%d]);' % (r9, r9, s, r9))
            return
        if (op & 0x1F0) == 0x100:  # sbcd / abcd
            fn = 'lift::sbcd' if top == 8 else 'lift::abcd'
            return _bcd(ctx, op, ins, code, fn)
        if top == 0xC and (op & 0x1F8) in (0x140, 0x148, 0x188):
            return _exg(ctx, op, code)
        B = SZ[sz]; to_ea = bool(op & 0x100)
        if B == 1 and mode == 1: raise Unsupported('byte An')
        check(mode, reg, 'alt' if to_ea else 'data')
        if to_ea and mode == 0: raise Unsupported('and/or ea=Dn')
        ea = Operand(ctx, code, mode, reg, B)
        o = 'lift::' if False else ''
        fn = '|' if top == 8 else '&'
        dn = 'c.d[%d]' % r9
        mask = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[B]
        if to_ea:
            r = ctx.t(); code.append('uint32_t %s = (%s %s %s) & 0x%X;' % (r, ea.read(), fn, dn, mask))
            code.append(ea.write(r)); ccr_logic(code, B, r)
        else:
            r = ctx.t(); code.append('uint32_t %s = (%s %s %s) & 0x%X;' % (r, ea.read(), fn, dn, mask))
            code.append('%s = lift::merge<%d>(%s, %s);' % (dn, B, dn, r)); ccr_logic(code, B, r)
        return
    if top in (9, 0xD):
        sz = (op >> 6) & 3
        isadd = top == 0xD
        if sz == 3:  # adda/suba
            B = 4 if op & 0x100 else 2
            src = Operand(ctx, code, mode, reg, B)
            v = ctx.t(); code.append('uint32_t %s = %s;' % (v, src.read() if B == 4 else 'lift::sx<2>(%s)' % src.read()))
            code.append('c.a[%d] %s= %s;' % (r9, '+' if isadd else '-', v)); return
        B = SZ[sz]
        if op & 0x130 == 0x100 and mode in (0, 1):  # addx/subx
            return _addx(ctx, op, ins, code, isadd, B)
        if op & 0x100: check(mode, reg, 'alt')
        if B == 1 and mode == 1: raise Unsupported('byte An')
        ea = Operand(ctx, code, mode, reg, B)
        fn = 'lift::add<%d>' % B if isadd else 'lift::sub<%d>' % B
        dn = 'c.d[%d]' % r9
        if op & 0x100:
            s = ctx.t(); d = ctx.t()
            code.append('uint32_t %s = %s; uint32_t %s = %s;' % (s, dn + ' & 0x%X' % {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[B], d, ea.read()))
            code.append(ea.write('%s(c, %s, %s)' % (fn, s, d)))
        else:
            s = ctx.t(); code.append('uint32_t %s = %s;' % (s, ea.read()))
            code.append('%s = lift::merge<%d>(%s, %s(c, %s, %s));' % (dn, B, dn, fn, s, dn))
        return
    if top == 0xB:
        sz = (op >> 6) & 3
        if sz == 3:  # cmpa
            B = 4 if op & 0x100 else 2
            src = Operand(ctx, code, mode, reg, B)
            code.append('lift::cmp<4>(c, %s, c.a[%d]);' % (src.read() if B == 4 else 'lift::sx<2>(%s)' % src.read(), r9)); return
        B = SZ[sz]
        if op & 0x100 and mode == 1:  # cmpm
            s = Operand(ctx, code, 3, reg, B); d = Operand(ctx, code, 3, r9, B)
            code.append('lift::cmp<%d>(c, %s, %s);' % (B, s.read(), d.read())); return
        if op & 0x100: check(mode, reg, 'alt')
        if B == 1 and mode == 1: raise Unsupported('byte An')
        ea = Operand(ctx, code, mode, reg, B)
        if op & 0x100:  # eor
            r = ctx.t(); code.append('uint32_t %s = (%s ^ c.d[%d]) & 0x%X;' % (r, ea.read(), r9, {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[B]))
            code.append(ea.write(r)); ccr_logic(code, B, r)
        else:
            code.append('lift::cmp<%d>(c, %s, c.d[%d]);' % (B, ea.read(), r9))
        return
    if top == 0xE:
        return _shift(ctx, op, ins, code)
    raise Unsupported('opcode %04x' % op)


def _exg(ctx, op, code):
    r9 = (op >> 9) & 7; reg = op & 7; m = op & 0x1F8
    if m == 0x140: code.append('std::swap(c.d[%d], c.d[%d]);' % (r9, reg))
    elif m == 0x148: code.append('std::swap(c.a[%d], c.a[%d]);' % (r9, reg))
    else: code.append('std::swap(c.d[%d], c.a[%d]);' % (r9, reg))


def _bcd(ctx, op, ins, code, fn):
    r9 = (op >> 9) & 7; reg = op & 7
    if op & 8:
        s = Operand(ctx, code, 4, reg, 1); d = Operand(ctx, code, 4, r9, 1)
        sv = ctx.t(); dv = ctx.t()
        code.append('uint32_t %s = %s; uint32_t %s = %s;' % (sv, s.read(), dv, d.read()))
        code.append(d.write('%s(c, %s, %s)' % (fn, sv, dv)))
    else:
        code.append('c.d[%d] = lift::merge<1>(c.d[%d], %s(c, c.d[%d] & 0xFF, c.d[%d] & 0xFF));' % (r9, r9, fn, reg, r9))


def _addx(ctx, op, ins, code, isadd, B):
    r9 = (op >> 9) & 7; reg = op & 7
    f = ('lift::add<%d>(c, %%s, %%s, true, true)' if isadd else 'lift::sub<%d>(c, %%s, %%s, true, true)') % B
    if op & 8:
        s = Operand(ctx, code, 4, reg, B); d = Operand(ctx, code, 4, r9, B)
        sv = ctx.t(); dv = ctx.t()
        code.append('uint32_t %s = %s; uint32_t %s = %s;' % (sv, s.read(), dv, d.read()))
        code.append(d.write(f % (sv, dv)))
    else:
        code.append('c.d[%d] = lift::merge<%d>(c.d[%d], %s);' % (r9, B, r9, f % ('c.d[%d]' % reg, 'c.d[%d]' % r9)))


def _shift(ctx, op, ins, code):
    sz = (op >> 6) & 3
    if sz == 3:  # memory shift, count 1
        if op & 0x800: raise Unsupported('68020 bitfield')
        kind = (op >> 9) & 3; left = bool(op & 0x100)
        check((op >> 3) & 7, op & 7, 'alt')
        if ((op >> 3) & 7) == 0: raise Unsupported('mem shift on dn')
        ea = Operand(ctx, code, (op >> 3) & 7, op & 7, 2)
        v = ctx.t(); code.append('uint32_t %s = %s;' % (v, ea.read()))
        code.append(ea.write('lift::shift<2>(c, %d, %s, %s, 1)' % (kind, 'true' if left else 'false', v))); return
    B = SZ[sz]; kind = (op >> 3) & 3; left = bool(op & 0x100); reg = op & 7; cnt = (op >> 9) & 7
    if op & 0x20: cexp = '(c.d[%d] & 63)' % cnt
    else: cexp = str(cnt or 8)
    code.append('c.d[%d] = lift::merge<%d>(c.d[%d], lift::shift<%d>(c, %d, %s, c.d[%d], %s));' % (reg, B, reg, B, kind, 'true' if left else 'false', reg, cexp))


def _group0(ctx, op, ins, code):
    mode = (op >> 3) & 7; reg = op & 7; r9 = (op >> 9) & 7
    if op & 0x100:  # bit ops with dn, or movep
        if mode == 1:
            B = 2 if not (op & 0x40) else 4
            d16 = s16(ctx.ext()); a = ctx.t()
            code.append('uint32_t %s = c.a[%d] + (%d);' % (a, reg, d16))
            n = 4 if (op & 0x40) else 2
            if op & 0x80:  # reg -> mem
                for i in range(n):
                    code.append('c.bus->w8(%s + %d, uint8_t(c.d[%d] >> %d));' % (a, 2 * i, r9, (n - 1 - i) * 8))
            else:
                parts = ' | '.join('(uint32_t(c.bus->r8(%s + %d)) << %d)' % (a, 2 * i, (n - 1 - i) * 8) for i in range(n))
                code.append('c.d[%d] = lift::merge<%d>(c.d[%d], %s);' % (r9, n, r9, parts))
            return
        _bit(ctx, op, code, 'c.d[%d]' % r9, mode, reg, dyn=True); return
    kind = (op >> 9) & 7
    if kind == 4:
        ext = ctx.ext()
        _bit(ctx, op, code, hx(ext), mode, reg, dyn=False); return
    sz = (op >> 6) & 3
    names = {0: 'or', 1: 'and', 2: 'sub', 3: 'add', 5: 'eor', 6: 'cmp'}
    if kind not in names or sz == 3: raise Unsupported('group0 %04x' % op)
    B = SZ[sz]
    if B == 4: imm = ctx.ext32()
    else: imm = ctx.ext() & (0xFF if B == 1 else 0xFFFF)
    if mode == 7 and reg == 4:  # to ccr / sr
        if B == 4 or kind in (2, 3, 6): raise Unsupported('imm to imm')
        if kind == 0: code.append('c.set_ccr(c.ccr() | %s);' % hx(imm & 0x1F)) if B == 1 else code.append('c.set_ccr(c.ccr() | %s); c.sr_sys |= %s;' % (hx(imm & 0x1F), hx(imm & 0xA700)))
        elif kind == 1: code.append('c.set_ccr(c.ccr() & %s);' % hx(imm & 0x1F)) if B == 1 else code.append('c.set_ccr(c.ccr() & %s); c.sr_sys &= %s;' % (hx(imm & 0x1F), hx(imm & 0xA700)))
        elif kind == 5: code.append('c.set_ccr(c.ccr() ^ %s);' % hx(imm & 0x1F))
        else: raise Unsupported('imm to ccr kind')
        return
    check(mode, reg, 'alt')
    ea = Operand(ctx, code, mode, reg, B)
    im = hx(imm)
    s = ctx.t(); code.append('uint32_t %s = %s;' % (s, ea.read()))
    if kind == 6: code.append('lift::cmp<%d>(c, %s, %s);' % (B, im, s)); return
    if kind == 2: code.append(ea.write('lift::sub<%d>(c, %s, %s)' % (B, im, s))); return
    if kind == 3: code.append(ea.write('lift::add<%d>(c, %s, %s)' % (B, im, s))); return
    op_ = {0: '|', 1: '&', 5: '^'}[kind]
    r = ctx.t(); code.append('uint32_t %s = (%s %s %s) & 0x%X;' % (r, s, op_, im, {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[B]))
    code.append(ea.write(r)); ccr_logic(code, B, r)


def _bit(ctx, op, code, bitexp, mode, reg, dyn):
    t = (op >> 6) & 3  # 0 btst 1 bchg 2 bclr 3 bset
    check(mode, reg, 'data' if t == 0 else 'alt')
    if not dyn and mode == 7 and reg >= 2: raise Unsupported('static btst pc/imm')
    if mode == 0:
        B = 4; ea = Operand(ctx, code, 0, reg, 4); m = '31'
    else:
        B = 1; ea = Operand(ctx, code, mode, reg, 1); m = '7'
    b = ctx.t(); v = ctx.t()
    code.append('uint32_t %s = (%s) & %s; uint32_t %s = %s; c.z = !((%s >> %s) & 1);' % (b, bitexp, m, v, ea.read(), v, b))
    if t == 1: code.append(ea.write('%s ^ (1u << %s)' % (v, b)))
    elif t == 2: code.append(ea.write('%s & ~(1u << %s)' % (v, b)))
    elif t == 3: code.append(ea.write('%s | (1u << %s)' % (v, b)))


def _group4(ctx, op, ins, code):
    mode = (op >> 3) & 7; reg = op & 7; r9 = (op >> 9) & 7; pc = ctx.pc
    if op == 0x4E71: return
    if op == 0x4E75:
        ins.flow = ('ret',); return
    if op == 0x4E73:
        ins.flow = ('rte',); return
    if op == 0x4E77:
        code.append('{ uint32_t f = c.bus->r16(c.a[7]); c.a[7] += 2; c.set_ccr(f); uint32_t t = c.bus->r32(c.a[7]); c.a[7] += 4; tgt = t; }'); ins.flow = ('jump_dyn',); return
    if op == 0x4E72: raise Unsupported('stop')
    if op == 0x4AFC: raise Unsupported('illegal')
    if op & 0xFFF0 == 0x4E40:
        code.append('return lift::Exit{%s, lift::Exit::Trap, %d};' % (hx(pc), op & 15)); ins.flow = ('stop',); return
    if op & 0xFFF8 == 0x4E50:
        d = s16(ctx.ext()); code.append('c.a[7] -= 4; c.bus->w32(c.a[7], c.a[%d]); c.a[%d] = c.a[7]; c.a[7] += (%d);' % (reg, reg, d)); return
    if op & 0xFFF8 == 0x4E58:
        code.append('c.a[7] = c.a[%d]; c.a[%d] = c.bus->r32(c.a[7]); c.a[7] += 4;' % (reg, reg)); return
    if op & 0xFFF8 == 0x4E60 or op & 0xFFF8 == 0x4E68: raise Unsupported('move usp')
    if (op & 0xFFC0) == 0x4EC0 or (op & 0xFFC0) == 0x4E80:
        call = (op & 0xFFC0) == 0x4E80
        check(mode, reg, 'mem_ctl')
        if mode in (2, 5, 6) or (mode == 7 and reg in (0, 1, 2, 3)):
            ea = Operand(ctx, code, mode, reg, 4)
            code.append('tgt = %s;' % ea.need_mem())
            if mode == 7 and reg in (0, 1, 2):
                # static target: find the constant
                ins.flow = ('call_dyn',) if call else ('jump_dyn',)
                ins.static = ea.ea
            else:
                ins.flow = ('call_dyn',) if call else ('jump_dyn',)
            return
        raise Unsupported('jmp ea')
    if (op & 0xF1C0) == 0x41C0:  # lea
        check(mode, reg, 'mem_ctl')
        ea = Operand(ctx, code, mode, reg, 4)
        code.append('c.a[%d] = %s;' % (r9, ea.need_mem())); return
    if (op & 0xFFC0) == 0x4840:
        if mode == 0:
            code.append('c.d[%d] = (c.d[%d] << 16) | (c.d[%d] >> 16); lift::flags_logic<4>(c, c.d[%d]);' % (reg, reg, reg, reg)); return
        check(mode, reg, 'mem_ctl')
        ea = Operand(ctx, code, mode, reg, 4)
        code.append('c.a[7] -= 4; c.bus->w32(c.a[7], %s);' % ea.need_mem()); return
    if (op & 0xFFB8) == 0x4880:  # ext
        if op & 0x40: code.append('c.d[%d] = uint32_t(int32_t(int16_t(c.d[%d]))); lift::flags_logic<4>(c, c.d[%d]);' % (reg, reg, reg))
        else: code.append('c.d[%d] = lift::merge<2>(c.d[%d], uint16_t(int16_t(int8_t(c.d[%d])))); lift::flags_logic<2>(c, c.d[%d]);' % (reg, reg, reg, reg))
        return
    if (op & 0xFB80) == 0x4880:  # movem
        to_reg = bool(op & 0x400); B = 4 if op & 0x40 else 2
        mask = ctx.ext()
        return _movem(ctx, op, code, to_reg, B, mask)
    if (op & 0xFFC0) == 0x40C0:  # move from sr
        ea = Operand(ctx, code, mode, reg, 2); code.append(ea.write('(c.sr_sys | c.ccr())')); return
    if (op & 0xFFC0) == 0x44C0:  # move to ccr
        ea = Operand(ctx, code, mode, reg, 2); code.append('c.set_ccr(%s);' % ea.read()); return
    if (op & 0xFFC0) == 0x46C0:  # move to sr
        ea = Operand(ctx, code, mode, reg, 2); v = ctx.t(); code.append('uint32_t %s = %s; c.set_ccr(%s); c.sr_sys = %s & 0xA700;' % (v, ea.read(), v, v)); return
    sz = (op >> 6) & 3
    h = op & 0xFF00
    if h in (0x4200, 0x4400, 0x4000, 0x4600, 0x4A00) and sz != 3:
        B = SZ[sz]
        check(mode, reg, 'alt')
        ea = Operand(ctx, code, mode, reg, B)
        if h == 0x4A00:
            v = ctx.t(); code.append('uint32_t %s = %s;' % (v, ea.read())); ccr_logic(code, B, v); return
        if h == 0x4200:
            code.append(ea.write('0')); code.append('c.n = false; c.z = true; c.v = false; c.c = false;'); return
        v = ctx.t(); code.append('uint32_t %s = %s;' % (v, ea.read()))
        if h == 0x4400: code.append(ea.write('lift::neg<%d>(c, %s)' % (B, v)))
        elif h == 0x4000: code.append(ea.write('lift::neg<%d>(c, %s, true)' % (B, v)))
        else:
            r = ctx.t(); code.append('uint32_t %s = (~%s) & 0x%X;' % (r, v, {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[B])); code.append(ea.write(r)); ccr_logic(code, B, r)
        return
    if (op & 0xFFC0) == 0x4AC0:  # tas
        check(mode, reg, 'alt'); ea = Operand(ctx, code, mode, reg, 1); v = ctx.t(); code.append('uint32_t %s = %s;' % (v, ea.read())); ccr_logic(code, 1, v); code.append(ea.write('%s | 0x80' % v)); return
    if (op & 0xFFC0) == 0x4800:  # nbcd
        check(mode, reg, 'alt'); ea = Operand(ctx, code, mode, reg, 1); v = ctx.t(); code.append('uint32_t %s = %s;' % (v, ea.read())); code.append(ea.write('lift::nbcd(c, %s)' % v)); return
    if (op & 0xF1C0) == 0x4180:  # chk
        raise Unsupported('chk')
    raise Unsupported('group4 %04x' % op)


def _movem(ctx, op, code, to_reg, B, mask):
    mode = (op >> 3) & 7; reg = op & 7
    check(mode, reg, 'movem_ld' if to_reg else 'movem_st')
    wr = lambda a, v: 'lift::wr<%d>(*c.bus, %s, %s);' % (B, a, v)
    if to_reg:
        if mode == 3:
            base = 'c.a[%d]' % reg; a = 'ma'; code.append('uint32_t ma = c.a[%d];' % reg)
        else:
            ea = Operand(ctx, code, mode, reg, 4); code.append('uint32_t ma = %s;' % ea.need_mem())
        for i in range(16):
            if mask & (1 << i):
                r = 'c.d[%d]' % i if i < 8 else 'c.a[%d]' % (i - 8)
                val = 'lift::rd<%d>(*c.bus, ma)' % B
                if B == 2: code.append('%s = lift::sx<2>(%s);' % (r, val))
                else: code.append('%s = %s;' % (r, val))
                code.append('ma += %d;' % B)
        if mode == 3: code.append('c.a[%d] = ma;' % reg)
    else:
        if mode == 4:
            code.append('uint32_t ma = c.a[%d];' % reg)
            for i in range(16):  # mask bit 0 = a7 ... bit 15 = d0
                if mask & (1 << i):
                    n = 15 - i
                    r = 'c.d[%d]' % n if n < 8 else 'c.a[%d]' % (n - 8)
                    # note: a7 written value is the initial value
                    rv = r if not (n == 15) else 'a7init'
                    if n == 15 + 0: pass
                    code.append('ma -= %d;' % B)
                    code.append(wr('ma', ('uint32_t(%s)' % r) if not (mode == 4 and reg == (n - 8) and n >= 8) else 'c.a[%d]' % reg))
            code.append('c.a[%d] = ma;' % reg)
        else:
            ea = Operand(ctx, code, mode, reg, 4); code.append('uint32_t ma = %s;' % ea.need_mem())
            for i in range(16):
                if mask & (1 << i):
                    r = 'c.d[%d]' % i if i < 8 else 'c.a[%d]' % (i - 8)
                    code.append(wr('ma', r)); code.append('ma += %d;' % B)
