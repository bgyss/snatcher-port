#include "debug_dump.h"

#include <cstdio>

namespace scd {

bool write_file(const std::string& path, const void* data, size_t len) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, len, f) == len;
    return std::fclose(f) == 0 && ok;
}

bool write_text(const std::string& path, const std::string& text) { return write_file(path, text.data(), text.size()); }

bool write_ppm(const std::string& path, const uint32_t* fb, int w, int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = fb[y * 320 + x];
            unsigned char rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
            std::fwrite(rgb, 1, 3, f);
        }
    const bool ok = !std::ferror(f);
    return std::fclose(f) == 0 && ok;
}

bool write_main_ram(const std::string& path, System& sys) { return write_file(path, sys.main_ram(), 0x10000); }
bool write_sub_ram(const std::string& path, System& sys) { return write_file(path, sys.prg_ram() + 0x7000, 0x6000); }

const char* verdict_name(WatchVerdict::Kind k) {
    switch (k) {
        case WatchVerdict::kHang: return "hang";
        case WatchVerdict::kHalted: return "halted";
        case WatchVerdict::kStillScreen: return "still";
        default: return "ok";
    }
}

bool write_failure_bundle(System& sys, const std::string& dir, const Replay& trace, const WatchVerdict& v) {
    uint32_t mpc, spc;
    sys.cpu_pcs(&mpc, &spc);
    const char* kind = verdict_name(v.kind);
    char json[320];
    std::snprintf(json, sizeof json, "{\"kind\":\"%s\",\"frame\":%llu,\"since\":%llu,\"e020\":\"0x%04x\",\"e022\":\"0x%04x\",\"e06c\":\"0x%04x\",\"main_pc\":\"0x%06x\",\"sub_pc\":\"0x%06x\"}\n",
                  kind, (unsigned long long)sys.frame_count(), (unsigned long long)v.since, sys.e020(), sys.e022(), sys.e06c(), mpc, spc);
    bool ok = write_text(dir + "/report.json", json);
    ok &= write_ppm(dir + "/final.ppm", sys.framebuffer(), sys.width(), sys.height());
    ok &= write_main_ram(dir + "/mainram.bin", sys);
    ok &= write_sub_ram(dir + "/subram.bin", sys);
    ok &= write_text(dir + "/repro.replay", trace.serialize());
    ok &= write_text(dir + "/state.txt", sys.state_text());
    sys.dump_state();
    debug_log("[watch] %s at frame %llu (e020=%04x main_pc=%06x sub_pc=%06x); bundle in %s\n", kind, (unsigned long long)sys.frame_count(),
              sys.e020(), mpc, spc, dir.c_str());
    return ok;
}

bool write_marker(System& sys, const std::string& dir, const std::string& time_iso) {
    uint32_t mpc, spc;
    sys.cpu_pcs(&mpc, &spc);
    char json[320];
    std::snprintf(json, sizeof json, "{\"frame\":%llu,\"e020\":\"0x%04x\",\"e022\":\"0x%04x\",\"e06c\":\"0x%04x\",\"main_pc\":\"0x%06x\",\"sub_pc\":\"0x%06x\",\"time\":\"%s\"}\n",
                  (unsigned long long)sys.frame_count(), sys.e020(), sys.e022(), sys.e06c(), mpc, spc, time_iso.c_str());
    bool ok = write_text(dir + "/marker.json", json);
    ok &= write_ppm(dir + "/screen.ppm", sys.framebuffer(), sys.width(), sys.height());
    ok &= write_main_ram(dir + "/mainram.bin", sys);
    ok &= write_sub_ram(dir + "/subram.bin", sys);
    ok &= write_text(dir + "/state.txt", sys.state_text());
    return ok;
}

}  // namespace scd
