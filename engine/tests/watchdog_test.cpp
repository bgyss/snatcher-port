#undef NDEBUG   // tests rely on assert even in Release builds
#include <cassert>
#include <cstdio>

#include "watchdog.h"

using namespace scd;

static WatchSample s(uint64_t f, uint16_t e020, uint64_t fb = 0, bool halted = false) { return {f, e020, fb, halted}; }

int main() {
    {   // counter advancing every frame: never fires
        Watchdog w;
        for (uint64_t f = 1; f < 5000; ++f) assert(w.feed(s(f, uint16_t(f), f)).kind == WatchVerdict::kOk);
    }
    {   // counter advancing slowly (once per 100 frames, CD-load pause): never fires
        Watchdog w;
        for (uint64_t f = 1; f < 5000; ++f) assert(w.feed(s(f, uint16_t(f / 100), f)).kind == WatchVerdict::kOk);
    }
    {   // counter stuck: fires, reports the stuck value
        Watchdog w;
        WatchVerdict v{};
        for (uint64_t f = 1; f <= 1000; ++f) {
            v = w.feed(s(f, f < 100 ? uint16_t(f) : 99, f));
            if (v.kind != WatchVerdict::kOk) break;
        }
        assert(v.kind == WatchVerdict::kHang && v.since == 99);
    }
    {   // CPU halted (illegal opcode) is reported immediately
        Watchdog w;
        assert(w.feed(s(1, 1, 0, true)).kind == WatchVerdict::kHalted);
    }
    {   // identical screen for still_frames while the counter advances: warns once, quiet until the screen changes
        Watchdog w;
        int warns = 0;
        for (uint64_t f = 1; f < 7000; ++f) warns += w.feed(s(f, uint16_t(f), 42)).kind == WatchVerdict::kStillScreen;
        assert(warns == 1);
    }
    std::puts("watchdog_test: ok");
    return 0;
}
