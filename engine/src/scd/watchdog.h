// Hang / crash / still-screen detector. Pure logic: scd_headless feeds it one sample per emulated frame.
#pragma once
#include <cstdint>

namespace scd {

struct WatchConfig { int hang_frames = 300; int still_frames = 1800; };
struct WatchSample { uint64_t frame; uint16_t e020; uint64_t fb_hash; bool halted; };
struct WatchVerdict {
    enum Kind { kOk, kHang, kHalted, kStillScreen } kind = kOk;
    uint64_t since = 0;   // kHang: the stuck $E020 value; kStillScreen: frame the screen last changed
};

class Watchdog {
public:
    explicit Watchdog(WatchConfig c = {}) : cfg_(c) {}
    WatchVerdict feed(const WatchSample& s) {
        if (s.halted) return {WatchVerdict::kHalted, s.frame};
        if (first_ || s.e020 != last_e020_) { last_e020_ = s.e020; e020_frame_ = s.frame; }
        if (first_ || s.fb_hash != last_fb_) { last_fb_ = s.fb_hash; fb_frame_ = s.frame; warned_ = false; }
        first_ = false;
        if (s.frame - e020_frame_ >= uint64_t(cfg_.hang_frames)) return {WatchVerdict::kHang, last_e020_};
        if (!warned_ && s.frame - fb_frame_ >= uint64_t(cfg_.still_frames)) { warned_ = true; return {WatchVerdict::kStillScreen, fb_frame_}; }
        return {};
    }
private:
    WatchConfig cfg_;
    bool first_ = true, warned_ = false;
    uint16_t last_e020_ = 0;
    uint64_t last_fb_ = 0, e020_frame_ = 0, fb_frame_ = 0;
};

}  // namespace scd
