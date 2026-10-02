#include "psg.h"

namespace scd {

void Psg::reset() {
    for (int i = 0; i < 3; ++i) tone_[i] = 0;
    for (int i = 0; i < 4; ++i) { vol_[i] = 0x0F; counter_[i] = 0; out_[i] = 0; }
    noise_ctl_ = 0;
    lfsr_ = 0x8000;
    latch_ = 0;
}

void Psg::write(uint8_t v) {
    if (v & 0x80) {
        latch_ = (v >> 4) & 7;
        int ch = latch_ >> 1;
        if (latch_ & 1) {
            vol_[ch] = v & 0x0F;
        } else if (ch < 3) {
            tone_[ch] = uint16_t((tone_[ch] & 0x3F0) | (v & 0x0F));
        } else {
            noise_ctl_ = v & 7;
            lfsr_ = 0x8000;
        }
    } else {
        int ch = latch_ >> 1;
        if (latch_ & 1) vol_[ch] = v & 0x0F;
        else if (ch < 3) tone_[ch] = uint16_t((tone_[ch] & 0x00F) | ((v & 0x3F) << 4));
    }
}

int16_t Psg::clock() {
    static const int16_t kVol[16] = {8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
                                     1298, 1031, 819, 650, 516, 410, 326, 0};
    for (int i = 0; i < 3; ++i) {
        if (counter_[i] > 0) --counter_[i];
        if (counter_[i] == 0) {
            counter_[i] = tone_[i] ? tone_[i] : 1;
            out_[i] ^= 1;
        }
    }
    if (counter_[3] > 0) --counter_[3];
    if (counter_[3] == 0) {
        int rate = noise_ctl_ & 3;
        counter_[3] = rate == 3 ? (tone_[2] ? tone_[2] : 1) : uint16_t(0x10 << rate);
        out_[3] ^= 1;
        if (out_[3]) {
            uint16_t fb;
            if (noise_ctl_ & 4) fb = ((lfsr_ ^ (lfsr_ >> 3)) & 1);
            else fb = lfsr_ & 1;
            lfsr_ = uint16_t((lfsr_ >> 1) | (fb << 15));
        }
    }
    int s = 0;
    for (int i = 0; i < 3; ++i) s += out_[i] ? kVol[vol_[i]] : -kVol[vol_[i]];
    s += (lfsr_ & 1) ? kVol[vol_[3]] : -kVol[vol_[3]];
    return int16_t(s / 2);
}

}  // namespace scd
