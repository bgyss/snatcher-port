#undef NDEBUG   // tests rely on assert even in Release builds
#include <cassert>
#include <cstdio>

#include "checkpoint.h"

using namespace scd;

int main() {
    assert(checkpoint_line(0x12c, 1, 0, {0x1, 0xab, 0xffffffffffffffffull}) ==
           "@0x12c scene=E022:0001/E06C:0000 vram=0000000000000001 ram=00000000000000ab fb=ffffffffffffffff\n");
    {   // scene changes and the every-N grid
        Checkpointer c(100);
        assert(c.due(1, 0, 0));      // first call: differs from the 0xFFFF sentinel, as scd_headless always did
        assert(!c.due(2, 0, 0));
        assert(c.due(3, 1, 0));      // $E022 changed
        assert(c.due(4, 1, 5));      // $E06C changed
        assert(!c.due(99, 1, 5));
        assert(c.due(100, 1, 5));    // grid
    }
    {   // every <= 0: scene changes only
        Checkpointer c(0);
        assert(c.due(1, 0, 0));
        assert(!c.due(300, 0, 0));
    }
    {   // one line per frame: the grid line and the exit line can land on the same frame
        Checkpointer c(100);
        const StateHash h{1, 2, 3};
        assert(!c.emit(100, 0, 0, h).empty());
        assert(c.emit(100, 0, 0, h).empty());
        assert(!c.emit(101, 0, 0, h).empty());
    }
    std::puts("checkpoint_test: ok");
    return 0;
}
