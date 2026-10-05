# Playtest Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deterministic record/replay of pad input, a hang/crash watchdog, and per-scene state checkpoints for `scd_headless`, plus a Python runner that compares two builds on one replay.

**Architecture:** A small `replay` module in `scdcore` (parse/serialize/lookup), two hooks at the top of `System::run_frame()` (play or record `pad_[0]`), a pure-logic `Watchdog` class fed once per frame by `scd_headless`, and a `StateHash` accessor on `System` used to write `.ckpt` files. `tools/playtest/` holds the runner and checkpoint differ. The input stream is keyed by boot frame count (`System::frame_count()`), the same numbering `--press` and the debug log already use.

**Tech Stack:** C++20 (CMake, existing `scdcore`/`scd_headless`), Python 3 (`.venv/bin/python`, stdlib `unittest`), mise tasks.

**Spec:** `docs/superpowers/specs/2026-10-05-playtest-harness-design.md`

## Global Constraints

- Never commit disc images, BIOS dumps, extracted data, listings, MAME captures, or generated translated C++. Replays and checkpoints contain only input events and hashes. `work/` is gitignored; write run output there.
- Build on macOS with Apple clang: `CC=/usr/bin/clang CXX=/usr/bin/clang++`. A cached CMake dir configured with another compiler must be replaced, not reconfigured.
- The safety hook blocks `rm -rf`. Use a fresh build directory or `mv`.
- No linter exists; correctness is judged by the tests in this plan and by determinism.
- Python tools run with `.venv/bin/python`.
- Translated and interpreted paths must stay bit-identical; nothing in this plan may change emulation behaviour (hooks only read state, except pad override during replay).
- In headless runs, build `--press` lists in bash (zsh does not word-split an unquoted `$VAR`).

## Review Focus

- A replay whose disc hash differs from the loaded disc: refuse with a clear error (Task 1 tests the hash check helper; Task 4 wires the refusal).
- A replay recorded with a different backup-RAM start state (the live app loads `bram.bin`, headless starts empty): this is a likely cause of the original replay divergence. The header records the BRAM hash and replay refuses on mismatch (Task 1, Task 4).
- Pad input recorded mid catch-up: the app calls `set_pad` once then may run several `run_frame()` calls; recording inside `run_frame()` makes the stream per-frame exact (Task 2 test).
- Empty or malformed replay file, unsorted frames, unknown button letters: parse must fail with a line number, not crash (Task 1).
- Watchdog must not fire on the 300-frame heartbeat gaps or CD-load pauses where `$E020` still advances; it must fire when `$E020` stalls (Task 5).
- A checkpoint file from a run that ended early (watchdog trip): the differ must report "run B ended at frame N" rather than a bogus mismatch (Task 7).

## File Structure

- Create `engine/src/scd/replay.h`, `engine/src/scd/replay.cpp`: `Replay` data type, text format, button helpers.
- Create `engine/src/scd/watchdog.h`: header-only pure logic.
- Modify `engine/src/scd/system.h`, `engine/src/scd/system.cpp`: replay hooks, `StateHash`, `cpu_pcs()`.
- Modify `engine/src/frontend/headless.cpp`: `--replay`, `--ckpt-out`, `--ckpt-every`, `--watch`, failure bundle, exit codes.
- Modify `engine/src/frontend/sdl_main.cpp`: `--record-input FILE`.
- Modify `engine/CMakeLists.txt`: add `replay.cpp` to `scdcore`; add `replay_test`, `watchdog_test`.
- Create `engine/tests/replay_test.cpp`, `engine/tests/watchdog_test.cpp`.
- Create `tools/playtest/run.py`, `tools/playtest/ckpt_diff.py`, `tools/playtest/press_to_replay.py`, `tools/playtest/test_playtest.py`.
- Create `tools/playtest/replays/` (committed replays + checkpoints), `docs/PLAYTEST.md`.
- Modify `mise.toml`: `playtest` and `test-playtest` tasks. Modify `docs/JOURNAL.md`, `docs/superpowers/specs/2026-10-05-playtest-harness-design.md` (keying decision).

---

### Task 1: Replay data type and text format

**Files:**
- Create: `engine/src/scd/replay.h`, `engine/src/scd/replay.cpp`
- Create: `engine/tests/replay_test.cpp`
- Modify: `engine/CMakeLists.txt` (add `src/scd/replay.cpp` to the `scdcore` sources; add `replay_test` next to `lift_diff`)
- Modify: `docs/superpowers/specs/2026-10-05-playtest-harness-design.md` (see Step 7)

**Interfaces:**
- Produces:
  - `uint16_t scd::buttons_from_string(const std::string&)` ("-" or "" gives 0; letters `UDLRBCAS`)
  - `std::string scd::buttons_to_string(uint16_t)` (fixed order `UDLRBCAS`, "-" for none)
  - `struct scd::Replay { std::string disc_sha1, engine, bram_sha1 = "none"; double cd_speed = 1.0; bool justifier = false; std::vector<ReplayEvent> events; void add(uint64_t frame, uint16_t buttons); uint16_t buttons_at(uint64_t frame) const; std::string serialize() const; static bool parse(const std::string& text, Replay* out, std::string* err); }`
  - `struct scd::ReplayEvent { uint64_t frame; uint16_t buttons; }`

- [ ] **Step 1: Write the failing test** `engine/tests/replay_test.cpp`

```cpp
#include <cassert>
#include <cstdio>
#include <string>

#include "replay.h"
#include "system.h"   // Button enum

using namespace scd;

static void test_roundtrip() {
    Replay r;
    r.disc_sha1 = "abc123";
    r.engine = "test";
    r.add(0x120, kUp);
    r.add(0x127, 0);
    r.add(0x600, kStart | kC);
    Replay p;
    std::string err;
    assert(Replay::parse(r.serialize(), &p, &err));
    assert(p.disc_sha1 == "abc123" && p.bram_sha1 == "none");
    assert(p.events.size() == 3 && p.events[2].frame == 0x600 && p.events[2].buttons == (kStart | kC));
}

static void test_lookup() {
    Replay r;
    r.add(10, kUp);
    r.add(20, 0);
    assert(r.buttons_at(0) == 0);
    assert(r.buttons_at(9) == 0);
    assert(r.buttons_at(10) == kUp);
    assert(r.buttons_at(19) == kUp);
    assert(r.buttons_at(20) == 0);
    assert(r.buttons_at(1000000) == 0);
}

static void test_add_dedups_equal_consecutive() {
    Replay r;
    r.add(5, kUp);
    r.add(6, kUp);   // no change: not stored
    r.add(7, 0);
    assert(r.events.size() == 2);
}

static void test_buttons() {
    assert(buttons_from_string("-") == 0 && buttons_from_string("") == 0);
    assert(buttons_from_string("SC") == (kStart | kC));
    assert(buttons_to_string(kUp | kDown) == "UD");
    assert(buttons_to_string(0) == "-");
}

static void test_parse_errors() {
    Replay p;
    std::string err;
    assert(!Replay::parse("", &p, &err));                                   // no header
    assert(!Replay::parse("# snatcher-replay v1\n@0x10 Z\n", &p, &err));    // bad button
    assert(err.find("line 2") != std::string::npos);
    assert(!Replay::parse("# snatcher-replay v1\n@0x20 U\n@0x10 -\n", &p, &err));   // unsorted
    assert(err.find("line 3") != std::string::npos);
    assert(!Replay::parse("# snatcher-replay v9\n", &p, &err));             // unknown version
}

int main() {
    test_roundtrip();
    test_lookup();
    test_add_dedups_equal_consecutive();
    test_buttons();
    test_parse_errors();
    std::puts("replay_test: ok");
    return 0;
}
```

- [ ] **Step 2: Add CMake targets and run to see it fail**

In `engine/CMakeLists.txt`, add `src/scd/replay.cpp` to the `add_library(scdcore ...)` list and, after the `scd_headless` target:

```cmake
add_executable(replay_test tests/replay_test.cpp)
target_link_libraries(replay_test PRIVATE scdcore)
```

Run: `CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S engine -B engine/build-pt && cmake --build engine/build-pt --target replay_test -j`
Expected: FAIL (replay.h not found).

- [ ] **Step 3: Implement** `engine/src/scd/replay.h`

```cpp
// Pad-input replay: which buttons are held from which boot frame (System::frame_count()). Text format, no disc data.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scd {

struct ReplayEvent { uint64_t frame; uint16_t buttons; };

uint16_t buttons_from_string(const std::string& s);
std::string buttons_to_string(uint16_t b);

struct Replay {
    std::string disc_sha1, engine, bram_sha1 = "none";
    double cd_speed = 1.0;
    bool justifier = false;
    std::vector<ReplayEvent> events;   // sorted by frame; buttons are held from `frame` until the next event

    void add(uint64_t frame, uint16_t buttons);          // ignores an event that repeats the current state
    uint16_t buttons_at(uint64_t frame) const;
    std::string serialize() const;
    static bool parse(const std::string& text, Replay* out, std::string* err);
};

}  // namespace scd
```

`engine/src/scd/replay.cpp`:

```cpp
#include "replay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "system.h"   // Button enum

namespace scd {
namespace {
const struct { uint16_t bit; char c; } kBtn[] = {{kUp, 'U'}, {kDown, 'D'}, {kLeft, 'L'}, {kRight, 'R'}, {kB, 'B'}, {kC, 'C'}, {kA, 'A'}, {kStart, 'S'}};
}

uint16_t buttons_from_string(const std::string& s) {
    uint16_t b = 0;
    for (char c : s)
        for (auto& k : kBtn) if (k.c == c) b |= k.bit;
    return b;
}

std::string buttons_to_string(uint16_t b) {
    std::string s;
    for (auto& k : kBtn) if (b & k.bit) s += k.c;
    return s.empty() ? "-" : s;
}

void Replay::add(uint64_t frame, uint16_t buttons) {
    uint16_t cur = events.empty() ? 0 : events.back().buttons;
    if (buttons == cur) return;
    events.push_back({frame, buttons});
}

uint16_t Replay::buttons_at(uint64_t frame) const {
    auto it = std::upper_bound(events.begin(), events.end(), frame, [](uint64_t f, const ReplayEvent& e) { return f < e.frame; });
    return it == events.begin() ? 0 : std::prev(it)->buttons;
}

std::string Replay::serialize() const {
    std::ostringstream o;
    o << "# snatcher-replay v1\n";
    o << "disc sha1=" << disc_sha1 << "\n";
    o << "bram sha1=" << bram_sha1 << "\n";
    o << "engine " << engine << "\n";
    o << "cdspeed " << cd_speed << "\n";
    o << "justifier " << (justifier ? "on" : "off") << "\n";
    for (auto& e : events) {
        char line[64];
        std::snprintf(line, sizeof line, "@0x%llx %s\n", (unsigned long long)e.frame, buttons_to_string(e.buttons).c_str());
        o << line;
    }
    return o.str();
}

bool Replay::parse(const std::string& text, Replay* out, std::string* err) {
    auto fail = [&](int line, const std::string& m) { if (err) *err = "line " + std::to_string(line) + ": " + m; return false; };
    std::istringstream in(text);
    std::string line;
    int n = 0;
    Replay r;
    bool header = false;
    while (std::getline(in, line)) {
        ++n;
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line != "# snatcher-replay v1") return fail(n, "unsupported or malformed header");
            header = true;
            continue;
        }
        if (!header) return fail(n, "missing '# snatcher-replay v1' header");
        if (line[0] == '@') {
            char* end;
            unsigned long long f = std::strtoull(line.c_str() + 1, &end, 0);
            if (end == line.c_str() + 1 || *end != ' ') return fail(n, "bad frame");
            std::string b = end + 1;
            for (char c : b) if (c != '-' && buttons_from_string(std::string(1, c)) == 0) return fail(n, std::string("unknown button '") + c + "'");
            if (!r.events.empty() && f <= r.events.back().frame) return fail(n, "frames must be strictly increasing");
            r.events.push_back({f, buttons_from_string(b)});
            continue;
        }
        std::istringstream ls(line);
        std::string key, val;
        ls >> key;
        std::getline(ls, val);
        if (!val.empty() && val[0] == ' ') val.erase(0, 1);
        if (key == "disc" || key == "bram") {
            if (val.rfind("sha1=", 0) != 0) return fail(n, "expected sha1=...");
            (key == "disc" ? r.disc_sha1 : r.bram_sha1) = val.substr(5);
        } else if (key == "engine") r.engine = val;
        else if (key == "cdspeed") r.cd_speed = std::atof(val.c_str());
        else if (key == "justifier") r.justifier = val == "on";
        else return fail(n, "unknown key '" + key + "'");
    }
    if (!header) return fail(1, "missing '# snatcher-replay v1' header");
    *out = std::move(r);
    return true;
}

}  // namespace scd
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build engine/build-pt --target replay_test -j && engine/build-pt/replay_test`
Expected: `replay_test: ok`

- [ ] **Step 5: Hash helper for identity checks.** Add to `replay.h` / `replay.cpp` a `std::string sha1_file(const std::string& path)` (return "none" if the file is missing). Implement SHA-1 in `replay.cpp` (about 40 lines, standard); add a test asserting `sha1_file` of a temp file holding `"abc"` is `a9993e364706816aba3e25717850c26c9cd0d89d`. Disc identity is the SHA-1 of the first `.bin` named in the cue's first `FILE` line (resolve relative to the cue). Add `std::string disc_sha1_from_cue(const std::string& cue)` doing that; no test needed beyond manual check against the redump hash.

- [ ] **Step 6: Run the test again, then commit**

```bash
engine/build-pt/replay_test
git add engine/src/scd/replay.h engine/src/scd/replay.cpp engine/tests/replay_test.cpp engine/CMakeLists.txt
git commit -m "Add replay format: parse/serialize/lookup with tests"
```

- [ ] **Step 7: Update the spec's keying decision.** In the spec, replace "keyed by game frame (`$E020`)" wherever it appears with "keyed by boot frame count (`System::frame_count()`, the numbering `--press` and the debug log use)", and add one line under "Replay file format": "`$E020` is not used as the key because it is 0 before the game starts and is not guaranteed monotonic across scenes." Commit with message "Spec: key replays by boot frame".

---

### Task 2: Replay hooks in `System::run_frame()`

**Files:**
- Modify: `engine/src/scd/system.h` (new public methods and two private pointers)
- Modify: `engine/src/scd/system.cpp:614` (`run_frame`)
- Test: `engine/tests/replay_test.cpp` (add a hook test)

**Interfaces:**
- Consumes: `Replay::buttons_at`, `Replay::add` (Task 1).
- Produces: `void System::set_replay_play(const Replay* r)`, `void System::set_replay_record(Replay* r)`. While a play replay is set, `pad_[0]` is overwritten at the top of every `run_frame()`. While a record replay is set, `rec->add(frames_, pad_[0])` runs at the top of every `run_frame()` after any override.

- [ ] **Step 1: Write the failing test.** The test needs a booted `System`, which needs a disc, so it cannot run in `replay_test`. Instead test the hook's contract with a tiny seam: add to `system.h` `uint16_t System::pad_for_test(int port) const { return pad_[port & 1]; }` and in `replay_test.cpp` do not boot at all. Replace this step with the end-to-end check in Step 5 (headless replays a press script and the debug log shows the same `[pad]` lines). Write that check now as `tools/playtest/test_playtest.py::test_replay_matches_press` (full code in Task 7; here only record that it is the failing test for this task, and run it after Step 4 to see it fail with "unrecognized argument --replay").

- [ ] **Step 2: Implement the hooks.** In `system.h` add `#include "replay.h"`, public:

```cpp
    // Replay hooks (run_frame start): play overrides port 1's pad, record logs port 1's pad per emulated frame.
    void set_replay_play(const Replay* r) { replay_play_ = r; }
    void set_replay_record(Replay* r) { replay_rec_ = r; }
```

private: `const Replay* replay_play_ = nullptr; Replay* replay_rec_ = nullptr;`

In `system.cpp`, at the start of `System::run_frame()` after the `if (halted_) return;` line:

```cpp
    if (replay_play_) set_pad(0, replay_play_->buttons_at(frames_));
    if (replay_rec_) replay_rec_->add(frames_, pad_[0]);
```

- [ ] **Step 3: Build.** `cmake --build engine/build-pt -j` (Expected: builds, `replay_test` still prints ok).

- [ ] **Step 4: Commit**

```bash
git add engine/src/scd/system.h engine/src/scd/system.cpp
git commit -m "System: replay play/record hooks at run_frame start"
```

---

### Task 3: `StateHash` and CPU PC accessor

**Files:**
- Modify: `engine/src/scd/system.h`, `engine/src/scd/system.cpp` (near `debug_frame_end`, ~1460)

**Interfaces:**
- Produces:
  - `struct scd::StateHash { uint64_t vram, ram, fb; };` and `StateHash System::state_hash()`
  - `void System::cpu_pcs(uint32_t* main_pc, uint32_t* sub_pc)` (24-bit PCs; same logic `debug_frame_end` uses today)
  - `uint16_t System::e020() const`, `e022()`, `e06c()` (big-endian words from `main_ram_`)

- [ ] **Step 1: Write the failing test.** Hashes need a booted system; the check is the determinism gate in Task 7 (`test_two_runs_identical`). Record it as the failing test: it fails now because `--ckpt-out` does not exist.

- [ ] **Step 2: Implement.** In `system.h`:

```cpp
struct StateHash { uint64_t vram, ram, fb; };
```
(before `class System`), and public methods:

```cpp
    StateHash state_hash();
    void cpu_pcs(uint32_t* main_pc, uint32_t* sub_pc);
    uint16_t e020() const { return uint16_t(main_ram_[0xE020] << 8 | main_ram_[0xE021]); }
    uint16_t e022() const { return uint16_t(main_ram_[0xE022] << 8 | main_ram_[0xE023]); }
    uint16_t e06c() const { return uint16_t(main_ram_[0xE06C] << 8 | main_ram_[0xE06D]); }
```

In `system.cpp` add a file-local FNV-1a over bytes, then:

```cpp
static uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

StateHash System::state_hash() {
    StateHash s;
    uint64_t h = fnv(vdp_.vram(), 0x10000);
    h = fnv(vdp_.cram(), 64 * 2, h);
    h = fnv(vdp_.vsram(), 64 * 2, h);
    for (int i = 0; i < 24; ++i) { uint8_t r = vdp_.reg(i); h = fnv(&r, 1, h); }
    s.vram = h;
    // RAM: all of Main RAM plus the Sub program/data window the debug dumps use. Tune this window if the determinism
    // gate shows false positives from uninitialised areas (record which bytes differ, then mask them).
    s.ram = fnv(prg_ram_ + 0x7000, 0x6000, fnv(main_ram_, sizeof main_ram_));
    s.fb = fnv(vdp_.framebuffer(), size_t(width()) * height() * sizeof(uint32_t));
    return s;
}

void System::cpu_pcs(uint32_t* main_pc, uint32_t* sub_pc) {
    Cpu keep = cur_;
    select_cpu(kMain);
    *main_pc = reg_get(M68K_REG_PC) & 0xFFFFFF;
    select_cpu(kSub);
    *sub_pc = reg_get(M68K_REG_PC) & 0xFFFFFF;
    select_cpu(keep);
}
```

Refactor `debug_frame_end` to call `cpu_pcs(&mpc, &spc)` instead of its inline block (behaviour unchanged).

- [ ] **Step 3: Build and confirm the heartbeat still prints** by running a 400-frame headless with `SCD_LOG=work/pt.log` and checking `grep -c '^\[hb\]' work/pt.log` is 1.

- [ ] **Step 4: Commit**

```bash
git add engine/src/scd/system.h engine/src/scd/system.cpp
git commit -m "System: state_hash, cpu_pcs and e020/e022/e06c accessors"
```

---

### Task 4: Headless `--replay`, `--ckpt-out`, `--ckpt-every`

**Files:**
- Modify: `engine/src/frontend/headless.cpp` (argument parsing near line 62; frame loop near line 113)

**Interfaces:**
- Consumes: `Replay::parse`, `sha1_file`, `disc_sha1_from_cue` (Task 1), `set_replay_play` (Task 2), `state_hash`, `e022`, `e06c` (Task 3).
- Produces CLI: `--replay FILE`, `--ckpt-out FILE`, `--ckpt-every N` (default 300), `--bram FILE` (alias for the `SCD_SAVE` env var). Checkpoint line format (exact): `@0x<hexframe> scene=E022:<4hex>/E06C:<4hex> vram=<16hex> ram=<16hex> fb=<16hex>`. `<hexframe>` is `sys.frame_count()` after the frame ran. Exit code 4 on replay identity mismatch.

- [ ] **Step 1: Write the failing test** (`tools/playtest/test_playtest.py`, skipped when no disc present; full harness arrives in Task 7). Add now:

```python
import os, subprocess, unittest, pathlib, hashlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"
HEADLESS = os.environ.get("SCD_HEADLESS", str(ROOT / "engine" / "build-pt" / "scd_headless"))
needs_disc = unittest.skipUnless(CUE.exists(), "disc not present")


def run_headless(*args, env=None):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([HEADLESS, str(CUE), *args], capture_output=True, text=True, env=e)


class TestCheckpoints(unittest.TestCase):
    @needs_disc
    def test_two_runs_identical(self):
        out = ROOT / "work" / "pt_test"
        out.mkdir(parents=True, exist_ok=True)
        a, b = out / "a.ckpt", out / "b.ckpt"
        for f in (a, b):
            r = run_headless("--frames", "400", "--out", str(out), "--ckpt-out", str(f), "--ckpt-every", "100")
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(a.read_text(), b.read_text())
        self.assertGreaterEqual(len(a.read_text().splitlines()), 4)
```

Run: `.venv/bin/python -m unittest tools.playtest.test_playtest -v` (create `tools/__init__.py`-free layout by running from `tools/playtest`: `cd tools/playtest && ../../.venv/bin/python -m unittest test_playtest -v`). Expected: FAIL (unknown option or missing file).

- [ ] **Step 2: Implement.** In `headless.cpp`: add `#include "replay.h"`; add variables `std::string replay_path, ckpt_path; int ckpt_every = 300; std::string bram;` and arg cases:

```cpp
        else if (a == "--replay" && i + 1 < argc) replay_path = argv[++i];
        else if (a == "--ckpt-out" && i + 1 < argc) ckpt_path = argv[++i];
        else if (a == "--ckpt-every" && i + 1 < argc) ckpt_every = std::atoi(argv[++i]);
        else if (a == "--bram" && i + 1 < argc) bram = argv[++i];
```

`--bram` feeds the `init(...)` save path (`bram.empty() ? env SCD_SAVE : bram`). The core writes the BRAM back on `shutdown()`, so the runner always passes a temp copy.

After `init`, load and verify the replay:

```cpp
    Replay replay;
    if (!replay_path.empty()) {
        FILE* rf = std::fopen(replay_path.c_str(), "rb");
        if (!rf) { std::fprintf(stderr, "error: cannot open %s\n", replay_path.c_str()); return 1; }
        std::string text; char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, rf)) > 0) text.append(buf, n);
        std::fclose(rf);
        if (!Replay::parse(text, &replay, &err)) { std::fprintf(stderr, "error: %s: %s\n", replay_path.c_str(), err.c_str()); return 1; }
        std::string dsha = disc_sha1_from_cue(cue);
        if (!replay.disc_sha1.empty() && replay.disc_sha1 != dsha) { std::fprintf(stderr, "error: replay was recorded for disc %s, this disc is %s\n", replay.disc_sha1.c_str(), dsha.c_str()); return 4; }
        std::string bsha = bram.empty() ? "none" : sha1_file(bram);
        if (replay.bram_sha1 != bsha) { std::fprintf(stderr, "error: replay needs backup RAM %s, got %s (pass --bram)\n", replay.bram_sha1.c_str(), bsha.c_str()); return 4; }
        sys.set_replay_play(&replay);
    }
```

Note: the BRAM hash is computed before the run (the core rewrites the file at shutdown), and `init` has already read it. If `init` is called before this block, move the `sha1_file(bram)` call above `init`.

Checkpoint writer, declared before the frame loop:

```cpp
    FILE* ckpt = nullptr;
    uint16_t last22 = 0xFFFF, last6c = 0xFFFF;
    if (!ckpt_path.empty() && !(ckpt = std::fopen(ckpt_path.c_str(), "w"))) { std::fprintf(stderr, "error: cannot open %s\n", ckpt_path.c_str()); return 1; }
    auto emit_ckpt = [&]() {
        StateHash h = sys.state_hash();
        std::fprintf(ckpt, "@0x%llx scene=E022:%04x/E06C:%04x vram=%016llx ram=%016llx fb=%016llx\n", (unsigned long long)sys.frame_count(),
                     sys.e022(), sys.e06c(), (unsigned long long)h.vram, (unsigned long long)h.ram, (unsigned long long)h.fb);
    };
```

In the loop, immediately after `sys.run_frame()` (and the recorder lines):

```cpp
        if (ckpt) {
            bool changed = sys.e022() != last22 || sys.e06c() != last6c;
            if (changed || (ckpt_every > 0 && sys.frame_count() % ckpt_every == 0)) emit_ckpt();
            last22 = sys.e022(); last6c = sys.e06c();
        }
```

After the loop, `if (ckpt) { emit_ckpt(); std::fclose(ckpt); }`.

When `--replay` is given, the existing `--press` code still sets `pad` before `run_frame`, but the replay hook overrides it; leave both.

- [ ] **Step 4: Build and run**

Run: `cmake --build engine/build-pt -j && cd tools/playtest && ../../.venv/bin/python -m unittest test_playtest -v`
Expected: `test_two_runs_identical ... ok`. If it fails with differing lines, the engine is not deterministic across two headless runs: STOP, print the first differing line (`diff a.ckpt b.ckpt | head`), and investigate with superpowers:systematic-debugging before continuing (suspects: uninitialised RAM, host time, `SCD_` env defaults, backup RAM file).

- [ ] **Step 5: Commit**

```bash
git add engine/src/frontend/headless.cpp tools/playtest/test_playtest.py
git commit -m "scd_headless: --replay, --ckpt-out, --ckpt-every, --bram; determinism test"
```

---

### Task 5: Watchdog (pure logic) and `--watch`

**Files:**
- Create: `engine/src/scd/watchdog.h`, `engine/tests/watchdog_test.cpp`
- Modify: `engine/CMakeLists.txt` (add `watchdog_test`), `engine/src/frontend/headless.cpp`

**Interfaces:**
- Produces:
  - `struct scd::WatchConfig { int hang_frames = 300; int still_frames = 1800; };`
  - `struct scd::WatchSample { uint64_t frame; uint16_t e020; uint64_t fb_hash; bool halted; };`
  - `struct scd::WatchVerdict { enum Kind { kOk, kHang, kHalted, kStillScreen } kind; uint64_t since; };`
  - `class scd::Watchdog { explicit Watchdog(WatchConfig = {}); WatchVerdict feed(const WatchSample&); }`. `kHang` and `kHalted` are fatal; `kStillScreen` is a warning returned once per still period.
- CLI: `--watch [--hang-frames N]`. Exit code 3 on a fatal verdict, after writing the failure bundle (Step 5).

- [ ] **Step 1: Write the failing test** `engine/tests/watchdog_test.cpp`

```cpp
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
    {   // counter stuck: fires exactly at hang_frames, reports where it got stuck
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
    {   // identical screen for still_frames while the counter advances: warns once, then stays quiet until the screen changes
        Watchdog w;
        int warns = 0;
        for (uint64_t f = 1; f < 7000; ++f) warns += w.feed(s(f, uint16_t(f), 42)).kind == WatchVerdict::kStillScreen;
        assert(warns == 1);
    }
    std::puts("watchdog_test: ok");
    return 0;
}
```

Add to CMake: `add_executable(watchdog_test tests/watchdog_test.cpp)`, `target_include_directories(watchdog_test PRIVATE src/scd)`.

- [ ] **Step 2: Run to verify it fails.** `cmake --build engine/build-pt --target watchdog_test` Expected: FAIL (watchdog.h missing).

- [ ] **Step 3: Implement** `engine/src/scd/watchdog.h`

```cpp
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build engine/build-pt --target watchdog_test && engine/build-pt/watchdog_test`
Expected: `watchdog_test: ok`

- [ ] **Step 5: Wire `--watch` into headless.** Include `watchdog.h`. Add `bool watch = false; int hang_frames = 300;` and args `--watch`, `--hang-frames N`. Before the loop: `Watchdog dog({hang_frames, 1800});` and a `std::vector<std::string> warnings;`. After `sys.run_frame()`:

```cpp
        if (watch) {
            StateHash h = sys.state_hash();
            WatchVerdict v = dog.feed({sys.frame_count(), sys.e020(), h.fb, sys.halted()});
            if (v.kind == WatchVerdict::kStillScreen) std::fprintf(stderr, "[watch] warning: screen unchanged since frame %llu\n", (unsigned long long)v.since);
            else if (v.kind != WatchVerdict::kOk) {
                write_failure_bundle(sys, out, replay_events_so_far, v);
                return 3;
            }
        }
```

`write_failure_bundle` (static function in `headless.cpp`) writes into `out`: `report.json` with `{"kind":"hang|halted","frame":N,"e020":"0x..","e022":"0x..","e06c":"0x..","main_pc":"0x..","sub_pc":"0x.."}` (PCs from `cpu_pcs`), `final.ppm` (existing `write_ppm`), `mainram.bin` and `subram.bin` (same slices `SCD_DUMP_AT` writes), and `repro.replay` (the replay serialized with the events up to the trip frame; when the run came from `--press`, convert those presses with `buttons_to_string`). Also call `dump_state()` so the log has both CPUs' state. Keep `state_hash()` computation per frame only when `--watch` is on (it hashes ~1 MB per frame; acceptable for a test mode).

- [ ] **Step 6: Verify the hang path with a synthetic hang.** Add to `test_playtest.py`:

```python
class TestWatch(unittest.TestCase):
    @needs_disc
    def test_hang_detected(self):
        out = ROOT / "work" / "pt_hang"
        out.mkdir(parents=True, exist_ok=True)
        # --hang-frames 1 trips immediately in the boot phase where $E020 is still 0: proves the exit code and bundle.
        r = run_headless("--frames", "50", "--out", str(out), "--watch", "--hang-frames", "5")
        self.assertEqual(r.returncode, 3, r.stderr)
        self.assertTrue((out / "report.json").exists())
        self.assertTrue((out / "final.ppm").exists())
```

Run: `cd tools/playtest && ../../.venv/bin/python -m unittest test_playtest.TestWatch -v`
Expected: PASS (`$E020` is 0 for the first frames, so a 5-frame threshold trips).

- [ ] **Step 7: Commit**

```bash
git add engine/src/scd/watchdog.h engine/tests/watchdog_test.cpp engine/CMakeLists.txt engine/src/frontend/headless.cpp tools/playtest/test_playtest.py
git commit -m "Watchdog: hang/halt/still-screen detection with failure bundle and --watch"
```

---

### Task 6: App `--record-input`

**Files:**
- Modify: `engine/src/frontend/sdl_main.cpp` (argument parsing near the other flags; exit path near line 170)

**Interfaces:**
- Consumes: `Replay`, `disc_sha1_from_cue`, `sha1_file`, `set_replay_record` (Tasks 1-2).
- Produces: `snatcher disc.cue --record-input FILE.replay` writes FILE.replay on exit and copies the backup RAM the session started with to `FILE.replay.bram`. The replay header records the disc SHA-1, the BRAM SHA-1 of that start file, the git rev or `unknown` in `engine`, the CD speed and the justifier flag.

- [ ] **Step 1: Implement.** Parse `--record-input FILE`. Right before `System::instance().init(...)`, copy the existing `bram.bin` (if it exists) to `FILE.replay.bram` and compute `sha1_file` of that copy (`"none"` when no file existed; then also write no `.bram` file). After init: `Replay rec; rec.disc_sha1 = disc_sha1_from_cue(cue); rec.bram_sha1 = ...; rec.cd_speed = cd_speed; rec.justifier = gun; System::instance().set_replay_record(&rec);`. On the normal exit path (where the app saves BRAM and closes the log), write `rec.serialize()` to the file and print `saved FILE`. Do the same on the error exit path if the init succeeded.

- [ ] **Step 2: Build the app target.** `cmake --build engine/build-pt -j` (the SDL target builds in this dir if SDL3 is found; otherwise use `engine/build`). Expected: compiles.

- [ ] **Step 3: Manual check (the user does this once).** Run `engine/build-pt/snatcher "<cue>" --record-input work/pt_manual.replay`, press a few buttons, quit. Then: `scd_headless <cue> --frames 600 --replay work/pt_manual.replay --bram work/pt_manual.replay.bram --ckpt-out work/pt_manual.ckpt` must exit 0 and `grep -c '^@' work/pt_manual.replay` must be greater than 0.

- [ ] **Step 4: Commit**

```bash
git add engine/src/frontend/sdl_main.cpp
git commit -m "App: --record-input writes a replay and the starting backup RAM"
```

---

### Task 7: Runner, checkpoint differ, press converter

**Files:**
- Create: `tools/playtest/ckpt_diff.py`, `tools/playtest/press_to_replay.py`, `tools/playtest/run.py`
- Modify: `tools/playtest/test_playtest.py`

**Interfaces:**
- Produces:
  - `ckpt_diff.parse(text) -> list[Ckpt]` (`Ckpt = (frame:int, scene:str, vram:str, ram:str, fb:str)`), `ckpt_diff.first_diff(a, b) -> dict | None`. The dict has keys `frame`, `scene`, `fields` (list of `vram`/`ram`/`fb` that differ) or `{"ended_early": "A"|"B", "frame": N}` when one run ends before the other. `fb` in `fields` means a visible glitch.
  - `press_to_replay.convert(presses: list[str], disc_sha1: str) -> str` for `FRAME:BUTTONS:HOLD` strings.
  - `run.py <replay> --headless BIN [--compare-headless BIN2] [--frames N] [--bram FILE] [--determinism]`. Exit 0 pass, 1 mismatch, 3 watchdog trip.

- [ ] **Step 1: Write the failing tests** (append to `test_playtest.py`)

```python
import sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import ckpt_diff, press_to_replay

A = "@0x64 scene=E022:0003/E06C:0000 vram=1 ram=2 fb=3\n@0xc8 scene=E022:0003/E06C:0001 vram=4 ram=5 fb=6\n"


class TestDiff(unittest.TestCase):
    def test_identical(self):
        self.assertIsNone(ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(A)))

    def test_fb_only(self):
        b = A.replace("fb=6", "fb=7")
        d = ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(b))
        self.assertEqual(d["frame"], 0xC8)
        self.assertEqual(d["fields"], ["fb"])

    def test_ended_early(self):
        short = A.splitlines()[0] + "\n"
        d = ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(short))
        self.assertEqual(d, {"ended_early": "B", "frame": 0xC8})


class TestConvert(unittest.TestCase):
    def test_press_list(self):
        text = press_to_replay.convert(["10:S:5", "30:UD:2"], "deadbeef")
        self.assertIn("disc sha1=deadbeef", text)
        self.assertIn("@0xa S", text)
        self.assertIn("@0xf -", text)
        self.assertIn("@0x1e UD", text)
        self.assertIn("@0x20 -", text)
```

Run: `cd tools/playtest && ../../.venv/bin/python -m unittest test_playtest.TestDiff test_playtest.TestConvert -v`
Expected: FAIL (modules missing).

- [ ] **Step 2: Implement** `ckpt_diff.py`

```python
"""Compare two checkpoint files (engine/src/frontend/headless.cpp --ckpt-out format)."""
import re
import sys
from typing import NamedTuple, Optional

LINE = re.compile(r"^@0x([0-9a-f]+) scene=(\S+) vram=([0-9a-f]+) ram=([0-9a-f]+) fb=([0-9a-f]+)$")


class Ckpt(NamedTuple):
    frame: int
    scene: str
    vram: str
    ram: str
    fb: str


def parse(text: str) -> list:
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        m = LINE.match(line)
        if not m:
            raise ValueError(f"line {n}: not a checkpoint: {line!r}")
        out.append(Ckpt(int(m[1], 16), m[2], m[3], m[4], m[5]))
    return out


def first_diff(a: list, b: list) -> Optional[dict]:
    for x, y in zip(a, b):
        if x.frame != y.frame:
            return {"frame": min(x.frame, y.frame), "scene": x.scene, "fields": ["frame"]}
        fields = [f for f in ("vram", "ram", "fb") if getattr(x, f) != getattr(y, f)]
        if fields or x.scene != y.scene:
            return {"frame": x.frame, "scene": x.scene, "fields": fields or ["scene"]}
    if len(a) != len(b):
        longer = a if len(a) > len(b) else b
        return {"ended_early": "B" if len(a) > len(b) else "A", "frame": longer[min(len(a), len(b))].frame}
    return None


if __name__ == "__main__":
    d = first_diff(parse(open(sys.argv[1]).read()), parse(open(sys.argv[2]).read()))
    print(d or "identical")
    sys.exit(1 if d else 0)
```

`press_to_replay.py`:

```python
"""Convert scd_headless --press lists (FRAME:BUTTONS:HOLD) into a .replay."""
import sys


def convert(presses, disc_sha1, bram_sha1="none"):
    ev = {}
    for p in presses:
        f, b, h = p.split(":")
        f, h = int(f), int(h)
        ev[f] = b
        ev.setdefault(f + h, "-")   # a later press starting at f+h wins
    lines = ["# snatcher-replay v1", f"disc sha1={disc_sha1}", f"bram sha1={bram_sha1}", "engine press_to_replay",
             "cdspeed 1.0", "justifier off"]
    last = "-"
    for f in sorted(ev):
        if ev[f] != last:
            lines.append(f"@0x{f:x} {ev[f]}")
            last = ev[f]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    print(convert(sys.argv[2:], sys.argv[1]), end="")
```

Check the `convert` test by hand against its assertions: presses `10:S:5` and `30:UD:2` give events at 10 (`S`), 15 (`-`), 30 (`UD`), 32 (`-`), which match `@0xa S`, `@0xf -`, `@0x1e UD`, `@0x20 -`.

- [ ] **Step 3: Run the diff and convert tests**

Run: `cd tools/playtest && ../../.venv/bin/python -m unittest test_playtest.TestDiff test_playtest.TestConvert -v`
Expected: PASS

- [ ] **Step 4: Implement the runner** `run.py`

```python
#!/usr/bin/env python3
"""Run a replay headless, optionally against a second build, and report the first divergence or watchdog trip."""
import argparse, pathlib, shutil, subprocess, sys, tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import ckpt_diff

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"


def run_once(headless, cue, replay, frames, bram, out):
    out.mkdir(parents=True, exist_ok=True)
    ckpt = out / "run.ckpt"
    cmd = [headless, str(cue), "--frames", str(frames), "--out", str(out), "--replay", str(replay),
           "--ckpt-out", str(ckpt), "--watch"]
    if bram:
        tmp = out / "bram.bin"
        shutil.copy(bram, tmp)   # the core writes BRAM back on exit, so never pass the original
        cmd += ["--bram", str(tmp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, ckpt, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--headless", required=True)
    ap.add_argument("--compare-headless")
    ap.add_argument("--cue", default=str(DEFAULT_CUE))
    ap.add_argument("--frames", type=int, default=20000)
    ap.add_argument("--bram")
    ap.add_argument("--determinism", action="store_true", help="run the same build twice and compare")
    ap.add_argument("--work", default=str(ROOT / "work" / "playtest"))
    a = ap.parse_args()
    work = pathlib.Path(a.work)
    other = a.compare_headless or (a.headless if a.determinism else None)

    rc, ck_a, err = run_once(a.headless, a.cue, a.replay, a.frames, a.bram, work / "A")
    if rc == 3:
        print(f"WATCHDOG TRIP in A; bundle in {work / 'A'}\n{err}")
        return 3
    if rc != 0:
        print(f"A failed (exit {rc}):\n{err}")
        return rc
    if other is None:
        print(f"ok: {len(ckpt_diff.parse(ck_a.read_text()))} checkpoints in {ck_a}")
        return 0
    rc, ck_b, err = run_once(other, a.cue, a.replay, a.frames, a.bram, work / "B")
    if rc == 3:
        print(f"WATCHDOG TRIP in B; bundle in {work / 'B'}\n{err}")
        return 3
    d = ckpt_diff.first_diff(ckpt_diff.parse(ck_a.read_text()), ckpt_diff.parse(ck_b.read_text()))
    if d:
        print(f"DIVERGED: {d}  (fb in fields = visible glitch)")
        return 1
    print("ok: builds match at every checkpoint")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: Add the end-to-end tests** to `test_playtest.py` (skipped without a disc):

```python
class TestRunner(unittest.TestCase):
    @needs_disc
    def test_replay_matches_press(self):
        # the same input expressed as --press and as a replay must produce identical checkpoints
        out = ROOT / "work" / "pt_press"
        out.mkdir(parents=True, exist_ok=True)
        rp = out / "p.replay"
        rp.write_text(press_to_replay.convert(["300:S:6", "500:C:6"], "", "none"))
        a, b = out / "a.ckpt", out / "b.ckpt"
        run_headless("--frames", "800", "--out", str(out), "--ckpt-out", str(a), "--press", "300:S:6", "--press", "500:C:6")
        run_headless("--frames", "800", "--out", str(out), "--ckpt-out", str(b), "--replay", str(rp))
        self.assertEqual(a.read_text(), b.read_text())
```

An empty `disc sha1=` skips the identity check (headless only checks when non-empty), which keeps this test independent of the disc hash.

Run: `cd tools/playtest && ../../.venv/bin/python -m unittest test_playtest -v`
Expected: all PASS (disc tests skip when no disc).

- [ ] **Step 6: Commit**

```bash
git add tools/playtest
git commit -m "playtest: runner, checkpoint differ, press converter, end-to-end tests"
```

---

### Task 8: First committed replay, mise tasks, docs, freeze repro

**Files:**
- Create: `tools/playtest/replays/intro_skip.replay`, `tools/playtest/replays/intro_skip.ckpt`
- Modify: `mise.toml`, `docs/JOURNAL.md`
- Create: `docs/PLAYTEST.md`

- [ ] **Step 1: Build the intro-skip replay from the scenario `tools/test_intro_skip.sh` already uses.** Read the script, take its `--press` list, and run `.venv/bin/python tools/playtest/press_to_replay.py "" <presses...> > tools/playtest/replays/intro_skip.replay` (disc hash left empty so it is portable between redump layouts; add a comment line `# disc hash intentionally empty` is NOT allowed by the parser, so omit it). Generate the checkpoint baseline: `engine/build-pt/scd_headless <cue> --frames <script frames> --replay tools/playtest/replays/intro_skip.replay --ckpt-out tools/playtest/replays/intro_skip.ckpt --out work/pt_baseline`.

- [ ] **Step 2: Verify determinism and both build modes.** Run `.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build-pt/scd_headless --determinism --frames <N>` (expected: `ok: builds match at every checkpoint`). Then build the interpreter-only binary (`cmake -S engine -B engine/build-interp-pt` with `CC/CXX` set to Apple clang) and run with `--compare-headless` pointing at it, with the translated build as A (the translated build is configured with `-DSNATCHER_TRANSLATE_DIR=$PWD/extracted/code`; if this repo's `engine/build-pt` was configured without it, configure a second dir). Expected: `ok`. Any `DIVERGED` here is a real translator-vs-interpreter finding: record it in `docs/JOURNAL.md` and stop to debug.

- [ ] **Step 3: Compare against the committed baseline.** `.venv/bin/python tools/playtest/ckpt_diff.py work/playtest/A/run.ckpt tools/playtest/replays/intro_skip.ckpt` must print `identical`.

- [ ] **Step 4: mise tasks.** Follow the format of the existing entries in `mise.toml` (read it first). Add `test-playtest` running `.venv/bin/python -m unittest discover -s tools/playtest -v` plus `engine/build-pt/replay_test` and `watchdog_test`, and `playtest` taking a replay path and invoking `run.py`.

- [ ] **Step 5: Write `docs/PLAYTEST.md`.** Cover: recording (`--record-input`), replaying (`--replay` and `--bram`), checkpoint files and what `fb` versus `ram` mismatches mean, the watchdog failure bundle contents, the determinism gate, the rule that replays and checkpoints are committable but `.replay.bram` files and `work/` output are not, and that phase 2 (the explorer) will write the same replay format. Add a short entry to `docs/JOURNAL.md` noting: (a) the live app loads `bram.bin` while headless starts empty, and the app applies pad input once per host iteration, so replays recorded from `--log` are only approximate; (b) the Engineering-exit freeze signature (Sub at `$D648` polling `$FF8003` bit 1, Main in the V-blank poll at `$FF0920`, `$E020` stuck at `0x4d11`, CD idle).

- [ ] **Step 6: Freeze repro (needs the user).** Ask the user to run the app with `--record-input tools/playtest/replays/engineering_freeze.replay` and reproduce the freeze (the starting `bram.bin` is copied automatically). Then run `run.py` on it with the translated build; the watchdog must exit 3 with a bundle. If headless does not freeze but the app did, the divergence is app-only (audio timing or frame pacing): compare the app's `--log` heartbeat frames against the headless run's `[hb]` lines and treat the first differing line as the lead. Once reproduced, debug with superpowers:systematic-debugging; the replay becomes a regression test that must flip from "expects trip" to "must pass" when fixed (store it under `tools/playtest/replays/known_bad/` until then, with a one-line README stating the expectation).

- [ ] **Step 7: Commit**

```bash
git add tools/playtest docs/PLAYTEST.md docs/JOURNAL.md mise.toml
git commit -m "playtest: intro-skip baseline replay, mise tasks, docs"
```

---

## Self-review notes

- **Spec coverage:** recorder (Tasks 1, 2, 6), `--replay` (4), watchdog hang/crash/still screen (5), checkpoints (3, 4), runner and compare mode (7), determinism gate (4, 7, 8), failure bundle (5), committed replay/known-bad case (8), docs (8). "Last 300 frames of state" from the spec is reduced to the final state dump plus `final.ppm`; add a rolling frame ring only if the first real failure shows it is needed.
- **Crash detection:** `System::illegal()` already sets `halted_` on unexpected opcodes (`system.cpp:986,1003`); the watchdog consumes `halted()`. Address-error vectors are not separately detected in phase 1.
- **Spec deviation recorded in Task 1 Step 7:** replays key on boot frame count instead of `$E020`.
- **Known tuning risk:** the `ram` hash window may include uninitialised bytes; Task 4 Step 4 is the gate that finds this.
