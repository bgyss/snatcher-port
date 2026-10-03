"""Flow glue shared by the test generator and the program generator."""
from m68k_decode import hx


def flow_code(ins):
    f = ins.flow; nxt = ins.pc + ins.size
    if f == 'next': return ['npc = %s;' % hx(nxt)]
    if f == ('self',): return []          # instruction code already set npc
    if f[0] == 'goto': return ['npc = %s;' % hx(f[1])]
    if f[0] == 'cond': return ['npc = lift::cond(c, %d) ? %s : %s;' % (f[1], hx(f[2]), hx(nxt))]
    if f[0] == 'call': return ['c.a[7] -= 4; c.bus->w32(c.a[7], %s); npc = %s;' % (hx(nxt), hx(f[1]))]
    if f[0] == 'jump_dyn': return ['npc = tgt;']
    if f[0] == 'call_dyn': return ['c.a[7] -= 4; c.bus->w32(c.a[7], %s); npc = tgt;' % hx(nxt)]
    if f[0] == 'ret': return ['npc = c.bus->r32(c.a[7]); c.a[7] += 4;']
    if f[0] == 'rte': return ['{ uint32_t f = c.bus->r16(c.a[7]); c.a[7] += 2; npc = c.bus->r32(c.a[7]); c.a[7] += 4; c.set_ccr(f); c.sr_sys = f & 0xA700; }']
    if f[0] == 'stop': return []
    raise ValueError(f)
