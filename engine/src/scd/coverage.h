// Fallback census (ADR-003 M1): which 68000 instructions ran in the Musashi interpreter, per CPU, code image and cause.
// Fed by System::trace_pc (the Musashi instruction hook), written by scd_headless when SCD_COVERAGE=<file> is set.
#pragma once
#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scd {

enum class Fallback : uint8_t {
    kInterpreter,     // translation is off (interpreter build or SCD_NO_TRANSLATE)
    kNotTranslated,   // no translated program covers this PC
    kCodeMismatch,    // a program covers it, but the bytes in memory differ from what was translated
    kIrqEntry,        // an interrupt is pending: Musashi takes the exception (translated code runs only with IRQs masked)
    kHleStub,         // a BIOS HLE trap stub or the rts after it
    kUnsupported,     // translated code stopped at an instruction it does not implement (rte, trap, stop, chk, ...)
    kCount
};

inline const char* fallback_name(Fallback f) {
    static const char* const kNames[] = {"interpreter", "not_translated", "code_mismatch", "irq_entry", "hle_stub", "unsupported"};
    return f < Fallback::kCount ? kNames[int(f)] : "?";
}

class Coverage {
public:
    void note(int cpu, uint32_t image, Fallback why, uint32_t pc) { ++hits_[key(cpu, image, why, pc)]; }

    uint64_t steps(Fallback why) const {
        uint64_t n = 0;
        for (auto& kv : hits_) if (why_of(kv.first) == why) n += kv.second;
        return n;
    }
    size_t distinct_pcs(Fallback why) const {
        size_t n = 0;
        for (auto& kv : hits_) if (why_of(kv.first) == why) ++n;
        return n;
    }

    std::string to_tsv(uint64_t frames, bool translated) const {
        std::vector<std::pair<uint64_t, uint64_t>> rows(hits_.begin(), hits_.end());
        std::sort(rows.begin(), rows.end());
        char line[96];
        std::snprintf(line, sizeof line, "# snatcher-coverage v1 frames=%" PRIu64 " translated=%d\n", frames, translated ? 1 : 0);
        std::string s = line;
        for (auto& r : rows) {
            std::snprintf(line, sizeof line, "%s\t%08x\t%s\t%06x\t%" PRIu64 "\n", (r.first >> 31 & 1) ? "sub" : "main",
                          uint32_t(r.first >> 32), fallback_name(why_of(r.first)), uint32_t(r.first & 0xFFFFFF), r.second);
            s += line;
        }
        return s;
    }

private:
    // image (32) | cpu (1) | reason (7) | pc (24): sorting the keys sorts by image, cpu, reason, pc.
    static uint64_t key(int cpu, uint32_t image, Fallback why, uint32_t pc) {
        return uint64_t(image) << 32 | uint64_t(cpu & 1) << 31 | uint64_t(why) << 24 | (pc & 0xFFFFFF);
    }
    static Fallback why_of(uint64_t k) { return Fallback(k >> 24 & 0x7F); }
    std::unordered_map<uint64_t, uint64_t> hits_;
};

}  // namespace scd
