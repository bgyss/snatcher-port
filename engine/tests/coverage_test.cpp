#undef NDEBUG   // tests rely on assert even in Release builds
#include <cassert>
#include <cstdio>
#include <string>

#include "coverage.h"

using namespace scd;

int main() {
    {   // empty census: header only
        Coverage c;
        assert(c.to_tsv(0, true) == "# snatcher-coverage v1 frames=0 translated=1\n");
        assert(c.steps(Fallback::kNotTranslated) == 0);
    }
    {   // counts accumulate per (cpu, image, reason, pc); rows sorted by image, cpu, reason, pc
        Coverage c;
        c.note(1, 0, Fallback::kNotTranslated, 0xD5CC);
        c.note(1, 0, Fallback::kNotTranslated, 0xD5CC);
        c.note(0, 0, Fallback::kIrqEntry, 0xFF0584);
        c.note(1, 0x1A2B3C4D, Fallback::kNotTranslated, 0x28010);
        assert(c.steps(Fallback::kNotTranslated) == 3);
        assert(c.distinct_pcs(Fallback::kNotTranslated) == 2);
        assert(c.steps(Fallback::kIrqEntry) == 1);
        assert(c.to_tsv(42, false) ==
               "# snatcher-coverage v1 frames=42 translated=0\n"
               "main\t00000000\tirq_entry\tff0584\t1\n"
               "sub\t00000000\tnot_translated\t00d5cc\t2\n"
               "sub\t1a2b3c4d\tnot_translated\t028010\t1\n");
    }
    {   // PCs are 24-bit
        Coverage c;
        c.note(0, 0, Fallback::kHleStub, 0xFF00036C);
        assert(c.to_tsv(1, true) == "# snatcher-coverage v1 frames=1 translated=1\nmain\t00000000\thle_stub\t00036c\t1\n");
    }
    {   // every reason has a name
        for (int i = 0; i < int(Fallback::kCount); ++i) assert(std::string(fallback_name(Fallback(i))) != "?");
    }
    std::puts("coverage_test: ok");
    return 0;
}
