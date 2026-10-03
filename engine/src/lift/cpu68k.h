// Runtime for build-time translated 68000 code: register file, flags, and instruction semantics.
// The generated C++ calls these helpers; the bus is supplied by the embedding (System or a test harness).
#pragma once
#include <cstdint>
#include <utility>

namespace lift {

struct Bus {
    virtual ~Bus() = default;
    virtual uint8_t r8(uint32_t a) = 0;
    virtual uint16_t r16(uint32_t a) = 0;
    virtual void w8(uint32_t a, uint8_t v) = 0;
    virtual void w16(uint32_t a, uint16_t v) = 0;
    uint32_t r32(uint32_t a) { return uint32_t(r16(a)) << 16 | r16(a + 2); }
    void w32(uint32_t a, uint32_t v) { w16(a, uint16_t(v >> 16)); w16(a + 2, uint16_t(v)); }
};

struct Exit {
    enum Kind { None, Unsupported, DivZero, Trap, Budget, NotTranslated };
    uint32_t pc = 0;
    Kind kind = None;
    int arg = 0;
};

struct Span { uint32_t lo, hi, crc; };

struct Cpu {
    uint32_t d[8] = {}, a[8] = {};   // a[7] is the active stack pointer
    bool x = false, n = false, z = false, v = false, c = false;
    uint16_t sr_sys = 0x2000;        // supervisor bits and interrupt mask
    Bus* bus = nullptr;

    uint32_t ccr() const { return (x << 4) | (n << 3) | (z << 2) | (v << 1) | uint32_t(c); }
    void set_ccr(uint32_t f) { x = f & 16; n = f & 8; z = f & 4; v = f & 2; c = f & 1; }
};

template <int B> struct Sz;
template <> struct Sz<1> { using U = uint8_t; using S = int8_t; static constexpr uint32_t mask = 0xFF, msb = 0x80; };
template <> struct Sz<2> { using U = uint16_t; using S = int16_t; static constexpr uint32_t mask = 0xFFFF, msb = 0x8000; };
template <> struct Sz<4> { using U = uint32_t; using S = int32_t; static constexpr uint32_t mask = 0xFFFFFFFF, msb = 0x80000000; };

template <int B> inline uint32_t rd(Bus& b, uint32_t a);
template <> inline uint32_t rd<1>(Bus& b, uint32_t a) { return b.r8(a); }
template <> inline uint32_t rd<2>(Bus& b, uint32_t a) { return b.r16(a); }
template <> inline uint32_t rd<4>(Bus& b, uint32_t a) { return b.r32(a); }
template <int B> inline void wr(Bus& b, uint32_t a, uint32_t v);
template <> inline void wr<1>(Bus& b, uint32_t a, uint32_t v) { b.w8(a, uint8_t(v)); }
template <> inline void wr<2>(Bus& b, uint32_t a, uint32_t v) { b.w16(a, uint16_t(v)); }
template <> inline void wr<4>(Bus& b, uint32_t a, uint32_t v) { b.w32(a, v); }

// Merge a sized result into a data register (upper bits preserved).
template <int B> inline uint32_t merge(uint32_t old, uint32_t v) {
    return B == 4 ? v : (old & ~Sz<B>::mask) | (v & Sz<B>::mask);
}
template <int B> inline uint32_t sx(uint32_t v) { return uint32_t(typename Sz<B>::S(v & Sz<B>::mask)); }

template <int B> inline void flags_logic(Cpu& c, uint32_t r) {
    r &= Sz<B>::mask;
    c.n = r & Sz<B>::msb; c.z = r == 0; c.v = false; c.c = false;
}
template <int B> inline uint32_t add(Cpu& c, uint32_t s, uint32_t d, bool with_x = false, bool keep_z = false) {
    uint64_t r = uint64_t(s & Sz<B>::mask) + (d & Sz<B>::mask) + (with_x ? c.x : 0);
    uint32_t res = uint32_t(r) & Sz<B>::mask;
    c.c = r > Sz<B>::mask; c.x = c.c;
    c.v = (~(s ^ d) & (s ^ res) & Sz<B>::msb) != 0;
    c.n = res & Sz<B>::msb;
    c.z = keep_z ? (c.z && res == 0) : res == 0;
    return res;
}
// dst - src
template <int B> inline uint32_t sub(Cpu& c, uint32_t s, uint32_t d, bool with_x = false, bool keep_z = false, bool set_x = true) {
    uint64_t r = uint64_t(d & Sz<B>::mask) - (s & Sz<B>::mask) - (with_x ? c.x : 0);
    uint32_t res = uint32_t(r) & Sz<B>::mask;
    bool borrow = (uint64_t(s & Sz<B>::mask) + (with_x ? c.x : 0)) > (d & Sz<B>::mask);
    c.c = borrow; if (set_x) c.x = borrow;
    c.v = ((s ^ d) & (d ^ res) & Sz<B>::msb) != 0;
    c.n = res & Sz<B>::msb;
    c.z = keep_z ? (c.z && res == 0) : res == 0;
    return res;
}
template <int B> inline uint32_t neg(Cpu& c, uint32_t d, bool with_x = false) { return sub<B>(c, d, 0, with_x, with_x); }
template <int B> inline void cmp(Cpu& c, uint32_t s, uint32_t d) { sub<B>(c, s, d, false, false, false); }

// Condition codes: 0 T,1 F,2 HI,3 LS,4 CC,5 CS,6 NE,7 EQ,8 VC,9 VS,10 PL,11 MI,12 GE,13 LT,14 GT,15 LE
inline bool cond(const Cpu& c, int cc) {
    switch (cc) {
        case 0: return true; case 1: return false;
        case 2: return !c.c && !c.z; case 3: return c.c || c.z;
        case 4: return !c.c; case 5: return c.c; case 6: return !c.z; case 7: return c.z;
        case 8: return !c.v; case 9: return c.v; case 10: return !c.n; case 11: return c.n;
        case 12: return c.n == c.v; case 13: return c.n != c.v;
        case 14: return !c.z && c.n == c.v; default: return c.z || c.n != c.v;
    }
}

// Shifts and rotates. kind: 0 AS, 1 LS, 2 ROX, 3 RO; left: direction; count already reduced by the caller (mod 64).
template <int B> inline uint32_t shift(Cpu& c, int kind, bool left, uint32_t val, uint32_t cnt) {
    constexpr int bits = B * 8;
    uint32_t m = Sz<B>::mask, v = val & m, res = v;
    bool carry = false;
    if (cnt == 0) {
        if (kind == 2) c.c = c.x; else c.c = false;
        c.n = v & Sz<B>::msb; c.z = v == 0; c.v = false;
        return v;
    }
    bool ovf = false;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (left) {
            carry = res & Sz<B>::msb;
            if (kind == 0) { uint32_t nr = (res << 1) & m; if ((nr ^ res) & Sz<B>::msb) ovf = true; res = nr; }
            else if (kind == 1) res = (res << 1) & m;
            else if (kind == 2) { res = ((res << 1) | c.x) & m; c.x = carry; }
            else res = ((res << 1) | carry) & m;
        } else {
            carry = res & 1;
            if (kind == 0) res = (res >> 1) | (res & Sz<B>::msb);
            else if (kind == 1) res >>= 1;
            else if (kind == 2) { res = (res >> 1) | (c.x ? Sz<B>::msb : 0); c.x = carry; }
            else res = (res >> 1) | (carry ? Sz<B>::msb : 0);
        }
    }
    (void)bits;
    c.c = carry;
    if (kind != 3 && kind != 2) c.x = carry;
    c.n = res & Sz<B>::msb; c.z = res == 0; c.v = (kind == 0) && ovf;
    return res;
}

// BCD: flag behaviour (including the undefined V/N) mirrors Musashi so translated code is bit-identical to the interpreter.
inline uint32_t abcd(Cpu& c, uint32_t s, uint32_t d) {
    uint32_t res = (s & 0xF) + (d & 0xF) + c.x;
    uint32_t v = ~res;
    if (res > 9) res += 6;
    res += (s & 0xF0) + (d & 0xF0);
    c.x = c.c = res > 0x99;
    if (c.c) res -= 0xA0;
    v &= res;
    c.v = (v & 0x80) != 0;
    c.n = (res & 0x80) != 0;
    res &= 0xFF;
    if (res) c.z = false;
    return res;
}
inline uint32_t sbcd(Cpu& c, uint32_t s, uint32_t d) {  // d - s - x
    uint32_t res = (d & 0xF) - (s & 0xF) - c.x;
    uint32_t v = ~res;
    if (int32_t(res) > 9 || res > 9) res -= 6;
    res += (d & 0xF0) - (s & 0xF0);
    c.x = c.c = res > 0x99;
    if (c.c) res += 0xA0;
    res &= 0xFF;
    v &= res;
    c.v = (v & 0x80) != 0;
    c.n = (res & 0x80) != 0;
    if (res) c.z = false;
    return res;
}
inline uint32_t nbcd(Cpu& c, uint32_t d) {
    uint32_t res = (0x9A - d - c.x) & 0xFF;
    if (res != 0x9A) {
        uint32_t v = ~res;
        if ((res & 0x0F) == 0xA) res = (res & 0xF0) + 0x10;
        res &= 0xFF;
        v &= res;
        c.v = (v & 0x80) != 0;
        if (res) c.z = false;
        c.c = c.x = true;
    } else {
        c.v = c.c = c.x = false;
    }
    c.n = (res & 0x80) != 0;
    return res;
}

}  // namespace lift
