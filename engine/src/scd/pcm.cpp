#include "pcm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace scd {

Pcm::Pcm() { reset(); }

void Pcm::reset() {
    std::memset(ram_, 0, sizeof ram_);
    for (auto& c : ch_) c = Channel();
    ctrl_ = 0;
    enable_ = 0xFF;
    bank_ = sel_ = 0;
    on_ = false;
}

uint8_t Pcm::read(uint32_t a) {
    a &= 0x3FFF;
    if (a & 0x2000) return ram_[((a >> 1) & 0xFFF) | (bank_ << 12)];
    int reg = (a >> 1) & 0x1F;
    // Address counters at $FF0020-$FF003F: per channel 4 bytes apart, low byte at +1 (offset 0x21 + 4n),
    // high byte at +3. Snatcher's stream driver ($15A00) reads exactly these to detect the end of a voice stream.
    if (reg >= 0x10 && reg < 0x20) {
        const Channel& c = ch_[(reg >> 1) & 7];
        return uint8_t((reg & 1) ? c.addr >> 19 : c.addr >> 11);
    }
    return 0xFF;
}

void Pcm::write(uint32_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a & 0x2000) {
        ram_[((a >> 1) & 0xFFF) | (bank_ << 12)] = v;
        return;
    }
    if (!(a & 1)) return;
    int reg = (a >> 1) & 0x1F;
    Channel& c = ch_[sel_];
    switch (reg) {
        case 0: c.env = v; break;
        case 1: c.pan = v; break;
        case 2: c.fd = uint16_t((c.fd & 0xFF00) | v); break;
        case 3: c.fd = uint16_t((c.fd & 0x00FF) | (v << 8)); break;
        case 4: c.ls = uint16_t((c.ls & 0xFF00) | v); break;
        case 5: c.ls = uint16_t((c.ls & 0x00FF) | (v << 8)); break;
        case 6: c.st = v; break;
        case 7:
            ctrl_ = v;
            on_ = v & 0x80;
            if (v & 0x40) sel_ = v & 7;
            else bank_ = v & 0x0F;
            break;
        case 8: {
            uint8_t turned_on = uint8_t(enable_ & ~v);
            enable_ = v;
            for (int i = 0; i < 8; ++i)
                if (turned_on & (1 << i)) ch_[i].addr = uint32_t(ch_[i].st) << 19;
            break;
        }
        default: break;
    }
}

std::string Pcm::describe() const {
    char line[160];
    std::string out;
    std::snprintf(line, sizeof line, "PCM on=%d ctrl=%02x bank=%d sel=%d enable=%02x\n", on_, ctrl_, bank_, sel_, enable_);
    out += line;
    for (int i = 0; i < 8; ++i) {
        std::snprintf(line, sizeof line, "  ch%d %s env=%02x pan=%02x fd=%04x ls=%04x st=%02x addr=%04x.%03x\n", i, (enable_ & (1 << i)) ? "off" : "ON ",
                      ch_[i].env, ch_[i].pan, ch_[i].fd, ch_[i].ls, ch_[i].st, ch_[i].addr >> 11, ch_[i].addr & 0x7FF);
        out += line;
    }
    return out;
}

void Pcm::clock(int16_t* left, int16_t* right) {
    int l = 0, r = 0;
    if (on_) {
        for (int i = 0; i < 8; ++i) {
            if (enable_ & (1 << i)) continue;
            Channel& c = ch_[i];
            uint8_t s = ram_[(c.addr >> 11) & 0xFFFF];
            if (s == 0xFF) {
                c.addr = uint32_t(c.ls) << 11;
                s = ram_[c.ls];
                if (s == 0xFF) continue;
            } else {
                c.addr = (c.addr + c.fd) & 0x07FFFFFF;
            }
            int v = (s & 0x80) ? (s & 0x7F) : -(s & 0x7F);
            l += (v * c.env * (c.pan & 0x0F)) >> 5;
            r += (v * c.env * (c.pan >> 4)) >> 5;
        }
    }
    *left = int16_t(std::clamp(l, -32768, 32767));
    *right = int16_t(std::clamp(r, -32768, 32767));
}

}  // namespace scd
