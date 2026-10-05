# Recomp M1: Coverage Census Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure every 68000 instruction that runs in the Musashi interpreter and why. Then close the gaps (overlays, the `$FFE100` Main RAM code, missed entry points) until nothing on the reference replays runs in the interpreter for want of translation.

**Architecture:**
- A header-only `Coverage` census in `scdcore` records (CPU, code image, cause, PC) for every Musashi instruction, through the existing instruction hook `System::trace_pc`. `scd_headless` writes it as TSV when `SCD_COVERAGE=<file>` is set.
- Overlays at `$28000` are identified by the CRC32 of their first 2 KB and dumped on first execution (`SCD_DUMP_OVERLAYS=<dir>`).
- `tools/lift/coverage_report.py` merges censuses, writes per-program seed files and gates (`--check`).
- `gen_program.py` gains seed files, region windows, a `mainram` program and generic `ovl:<file>` programs with a generated registry. The runtime picks overlay programs from that registry and runs Main through the same per-range CRC machinery as the Sub.

**Tech Stack:** C++20 (CMake, `scdcore`, `scd_headless`), Python 3 (`.venv/bin/python`, stdlib `unittest`), mise tasks, bash.

**Spec:** `docs/ADR-003-static-recompilation.md` (milestone M1). Background: `docs/ADR-002-build-time-translator.md`, `docs/JOURNAL.md`.

## Global Constraints

- Never commit disc images, BIOS dumps, extracted data, listings, MAME captures, or generated translated C++. Coverage censuses, overlay dumps and seed files are disc-derived: they live in `work/` or `extracted/code/` (both gitignored) and are never committed.
- Build on macOS with Apple clang: `CC=/usr/bin/clang CXX=/usr/bin/clang++`. A cached CMake dir configured with another compiler must be replaced, not reconfigured.
- The safety hook blocks `rm -rf`. Use a fresh build directory or `mv`. In Python tests use `Path.unlink()`.
- Python tools run with `.venv/bin/python`.
- Translated and interpreted paths must stay bit-identical. `engine/build/lift_diff 8` must report 0 failing cases after every task that touches the translator or runtime.
- Census and overlay tracking must not change emulation. They only read state, and are off unless their environment variable is set.
- In headless runs, build `--press` lists in bash (zsh does not word-split an unquoted `$VAR`).
- No linter exists.

## Review Focus

- `SCD_COVERAGE` together with `SCD_LIFT_VERIFY`: verify mode replays every translated instruction in Musashi, which would flood the census with false "interpreter" steps. The census must refuse, say so, and write no file. (Task 2 test `test_lift_verify_disables_census`.)
- Two different overlays executed at the same `$28000` addresses (same first 2 KB, different code further on): the runtime must not run the wrong translation. The per-range CRC must route it to the interpreter with cause `code_mismatch`. (Task 5 test `test_same_pc_different_bytes_gets_different_range_crc`. Task 6 selects the first program whose CRC matches, not the first that claims the PC.)
- Main RAM code at `$FFE100` overwritten after its CRC was checked: the per-range state must be invalidated by writes to that window. (Task 6, `main_code_gen_`, checked by the Task 7 gate: no `code_mismatch` rows for `mainram` and no `not_translated` rows inside `$FFE100..$FFE3D0`.)
- Seed files that are stale or from another disc revision, with addresses outside the program: ignored without error. (Task 5 test `test_out_of_range_seed_is_ignored`.)
- Running the census twice must accumulate seeds (union), not replace them, because each replay covers a different part of the game. (Task 4 test `test_seeds_accumulate_across_runs`.)
- A watchdog trip (exit 3) must still write the census, because the hang is exactly where coverage matters. (Task 2 writes the census before both `sys.shutdown()` calls in `headless.cpp`.)

---

### Task 1: Coverage census data structure

**Files:**
- Create: `engine/src/scd/coverage.h`
- Create: `engine/tests/coverage_test.cpp`
- Modify: `engine/CMakeLists.txt` (after the `watchdog_test` target)
- Modify: `mise.toml` (new `test-recomp` task)

**Interfaces:**
- Produces:
  - `enum class scd::Fallback : uint8_t { kInterpreter, kNotTranslated, kCodeMismatch, kIrqEntry, kHleStub, kUnsupported, kCount }`
  - `const char* scd::fallback_name(Fallback)`, which returns `"interpreter"`, `"not_translated"`, `"code_mismatch"`, `"irq_entry"`, `"hle_stub"` or `"unsupported"`
  - `class scd::Coverage` with:
    - `void note(int cpu, uint32_t image, Fallback why, uint32_t pc)` (cpu 0 = Main, 1 = Sub)
    - `uint64_t steps(Fallback) const`
    - `size_t distinct_pcs(Fallback) const`
    - `std::string to_tsv(uint64_t frames, bool translated) const`
  - TSV format: the header line `# snatcher-coverage v1 frames=<N> translated=<0|1>`, then rows `<main|sub>\t<image %08x>\t<reason>\t<pc %06x>\t<count>`, sorted by (image, cpu, reason, pc).

- [ ] **Step 1: Write the failing test**

`engine/tests/coverage_test.cpp`:

```cpp
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
```

Add to `engine/CMakeLists.txt` directly after the `watchdog_test` lines:

```cmake
add_executable(coverage_test tests/coverage_test.cpp)
target_include_directories(coverage_test PRIVATE src/scd)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S engine -B engine/build-interp -DCMAKE_BUILD_TYPE=Release && cmake --build engine/build-interp --target coverage_test`
Expected: compile error, `'coverage.h' file not found`.

- [ ] **Step 3: Write the implementation**

`engine/src/scd/coverage.h`:

```cpp
// Fallback census (ADR-003 M1): which 68000 instructions ran in the Musashi interpreter, per CPU, code image and cause.
// Fed by System::trace_pc (the Musashi instruction hook), written by scd_headless when SCD_COVERAGE=<file> is set.
#pragma once
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
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
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build engine/build-interp --target coverage_test && engine/build-interp/coverage_test`
Expected: `coverage_test: ok`

- [ ] **Step 5: Add the mise task**

Append to `mise.toml`:

```toml
[tasks.test-recomp]
description = "ADR-003 tests: coverage census, coverage report, translator seeds and overlays"
run = [
  "cmake -S engine -B engine/build-interp -DCMAKE_BUILD_TYPE=Release",
  "cmake --build engine/build-interp -j --target scd_headless coverage_test",
  "engine/build-interp/coverage_test",
]
```

Run: `mise run test-recomp`
Expected: ends with `coverage_test: ok`.

- [ ] **Step 6: Commit**

```bash
git add engine/src/scd/coverage.h engine/tests/coverage_test.cpp engine/CMakeLists.txt mise.toml
git commit -m "recomp M1: fallback census data structure (ADR-003)"
```

---

### Task 2: Record the census in System, write it from scd_headless

**Files:**
- Modify: `engine/src/scd/system.h`
- Modify: `engine/src/scd/system.cpp` (`init` near line 136, `trace_pc` at line 348, `run_cpu` at line 590, `run_sub_translated` at lines 477-544, `run_main_translated` at lines 548-587)
- Modify: `engine/src/frontend/headless.cpp` (both `sys.shutdown()` call sites, around lines 205 and 297)
- Create: `tools/lift/test_coverage.py`
- Modify: `mise.toml` (`test-recomp`)

**Interfaces:**
- Consumes: `scd::Coverage`, `scd::Fallback` (Task 1).
- Produces:
  - `bool System::write_coverage(const std::string& path)`. It returns false, with a log line containing `census disabled`, when the census is off because `SCD_LIFT_VERIFY` is set.
  - Members `Fallback fallback_why_`, `Coverage cov_`, `bool cov_on_`, `uint32_t ovl_image_ = 0`. Task 3 sets `ovl_image_`.
  - Env `SCD_COVERAGE=<file>`.
- Behaviour change: after a translated run exits with `Exit::NotTranslated`, the Sub and Main loops re-dispatch at `ex.pc` instead of stepping Musashi unconditionally. Another program may own that PC. The interpreter still runs it when none does.

- [ ] **Step 1: Write the failing end-to-end test**

`tools/lift/test_coverage.py`:

```python
"""End-to-end tests for the fallback census (ADR-003 M1). Skips without the disc or the builds."""
import os, pathlib, subprocess, unittest, zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"
INTERP = pathlib.Path(os.environ.get("SCD_HEADLESS_INTERP", ROOT / "engine" / "build-interp" / "scd_headless"))
TRANS = pathlib.Path(os.environ.get("SCD_HEADLESS_TR", ROOT / "engine" / "build" / "scd_headless"))
OUT = ROOT / "work" / "cov_test"


def run(binary, frames, **env):
    OUT.mkdir(parents=True, exist_ok=True)
    e = dict(os.environ)
    for k in ("SCD_NO_TRANSLATE", "SCD_LIFT_VERIFY", "SCD_COVERAGE", "SCD_DUMP_OVERLAYS"):
        e.pop(k, None)
    e.update(env)
    r = subprocess.run([str(binary), str(CUE), "--frames", str(frames), "--out", str(OUT)], capture_output=True, text=True, env=e)
    assert r.returncode == 0, r.stderr[-2000:]
    return r


def rows(path):
    return [l.split("\t") for l in path.read_text().splitlines() if l and not l.startswith("#")]


@unittest.skipUnless(CUE.exists() and INTERP.exists(), "disc or interpreter build missing")
class TestInterpreterCensus(unittest.TestCase):
    def test_every_step_is_interpreter(self):
        f = OUT / "interp.tsv"
        run(INTERP, 400, SCD_COVERAGE=str(f))
        self.assertTrue(f.read_text().startswith("# snatcher-coverage v1 frames=400 translated=0\n"))
        r = rows(f)
        self.assertEqual({x[2] for x in r}, {"interpreter"})
        self.assertEqual({x[0] for x in r}, {"main", "sub"})


@unittest.skipUnless(CUE.exists() and TRANS.exists(), "disc or translated build missing")
class TestTranslatedCensus(unittest.TestCase):
    def test_no_interpreter_rows_and_irq_entries_present(self):
        f = OUT / "tr.tsv"
        run(TRANS, 400, SCD_COVERAGE=str(f))
        self.assertIn("translated=1", f.read_text().splitlines()[0])
        reasons = {x[2] for x in rows(f)}
        self.assertNotIn("interpreter", reasons)
        self.assertIn("irq_entry", reasons)

    def test_lift_verify_disables_census(self):
        f = OUT / "verify.tsv"
        f.unlink(missing_ok=True)
        r = run(TRANS, 60, SCD_COVERAGE=str(f), SCD_LIFT_VERIFY="1")
        self.assertFalse(f.exists())
        self.assertIn("census disabled", r.stderr)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build engine/build-interp -j --target scd_headless && .venv/bin/python tools/lift/test_coverage.py -k Interpreter`
Expected: FAIL. `interp.tsv` does not exist (`FileNotFoundError`).

- [ ] **Step 3: Add the members to `system.h`**

Add `#include "coverage.h"` after `#include "disc.h"`.

In the public "Debug." section, after `void dump_overlay_hist();`, add:

```cpp
    // SCD_COVERAGE census (ADR-003): writes the TSV, false when the census is off (SCD_LIFT_VERIFY) or the file cannot be written.
    bool write_coverage(const std::string& path);
```

In the private section, after `std::map<uint32_t, uint64_t> ovl_hist_;`, add:

```cpp
    Coverage cov_;
    bool cov_on_ = false;
    Fallback fallback_why_ = Fallback::kInterpreter;   // cause of the Musashi steps about to run (read by trace_pc)
    uint32_t ovl_image_ = 0;                          // CRC32 of the first 2 KB at $28000 for the current overlay (Task 3)
```

- [ ] **Step 4: Enable it in `System::init`**

After `ovl_log_ = std::getenv("SCD_OVERLAYS") != nullptr;` add:

```cpp
    // The census is meaningless under SCD_LIFT_VERIFY: verify replays every translated instruction in Musashi.
    cov_on_ = std::getenv("SCD_COVERAGE") != nullptr && std::getenv("SCD_LIFT_VERIFY") == nullptr;
```

- [ ] **Step 5: Record in `trace_pc`**

At the top of `void System::trace_pc(uint32_t pc)`, before the `ovl_log_` line, add:

```cpp
    if (cov_on_) {
        uint32_t p = pc & 0xFFFFFF;
        Fallback why = fallback_why_;
        if (why == Fallback::kNotTranslated && (hle_.count(p) || hle_.count(p - 2))) why = Fallback::kHleStub;   // trap word, or the rts after it
        cov_.note(int(cur_), cur_ == kSub && p >= 0x28000 && p < 0x80000 ? ovl_image_ : 0, why, p);
    }
```

- [ ] **Step 6: Tag the cause of each Musashi step**

In `System::run_cpu`, directly before `#ifdef SNATCHER_TRANSLATED` (the dispatch), add:

```cpp
    fallback_why_ = translate_ ? Fallback::kNotTranslated : Fallback::kInterpreter;   // whole-slice Musashi runs (no translated program for this CPU)
```

In `run_sub_translated`, replace:

```cpp
        TEntry* te = nullptr;
        if (irq_blocked) {
            for (auto& e : sub_entries()) if (e.has(pc)) { te = &e; break; }
        }
        if (te) {
            for (int i = 0; i < 8; ++i) { c.d[i] = reg_get(M68K_REG_D0 + i); c.a[i] = i < 7 ? reg_get(M68K_REG_A0 + i) : reg_get(M68K_REG_SP); }
            c.set_ccr(sr & 0x1F);
            c.sr_sys = uint16_t(sr & 0xA700);
            // The code bytes in memory must be the ones that were translated (overlays are re-checked after every disc load into PRG RAM).
            bool ok = entry_code_ok(*te, prg_ram_, pc, te->overlay ? prg_ovl_gen_ : 0);
            if (ok) {
```

with:

```cpp
        // First program whose code at pc is in memory exactly as translated (overlays are re-checked after every disc load into PRG RAM).
        TEntry* te = nullptr;
        bool claimed = false;
        if (irq_blocked) {
            for (auto& e : sub_entries()) {
                if (!e.has(pc)) continue;
                claimed = true;
                if (entry_code_ok(e, prg_ram_, pc, e.overlay ? prg_ovl_gen_ : 0)) { te = &e; break; }
            }
        }
        fallback_why_ = !irq_blocked ? Fallback::kIrqEntry : claimed ? Fallback::kCodeMismatch : Fallback::kNotTranslated;
        if (te) {
            for (int i = 0; i < 8; ++i) { c.d[i] = reg_get(M68K_REG_D0 + i); c.a[i] = i < 7 ? reg_get(M68K_REG_A0 + i) : reg_get(M68K_REG_SP); }
            c.set_ccr(sr & 0x1F);
            c.sr_sys = uint16_t(sr & 0xA700);
            {
```

Then replace the end of that block:

```cpp
                if (ex.kind == lift::Exit::Budget) break;
                if (ex.pc != pc || ex.kind != lift::Exit::NotTranslated) { /* fall through to a fallback step at ex.pc */ }
            }
        }
```

with:

```cpp
                if (ex.kind == lift::Exit::Budget) break;
                if (ex.kind == lift::Exit::NotTranslated) continue;   // re-dispatch: ex.pc may belong to another program, else the interpreter runs it
                fallback_why_ = Fallback::kUnsupported;               // Unsupported / Trap / DivZero: Musashi executes the instruction at ex.pc
            }
        }
```

The inner `{` keeps the existing indentation of the verify and run code unchanged. Task 6 rewrites this function's dispatch again for the overlay registry.

In `run_main_translated`, before `++fallback_steps_;`, add the cause for each path. Replace the `if (irq_blocked && lift::main_has(pc)) {` block's two exits:

```cpp
                if (ex.kind == lift::Exit::Budget) break;
                if (halted_) break;
            }
        }
```

with:

```cpp
                if (ex.kind == lift::Exit::Budget) break;
                if (halted_) break;
                if (ex.kind == lift::Exit::NotTranslated) continue;
                fallback_why_ = Fallback::kUnsupported;
            } else {
                fallback_why_ = Fallback::kCodeMismatch;
            }
        } else {
            fallback_why_ = !irq_blocked ? Fallback::kIrqEntry : Fallback::kNotTranslated;
        }
```

Check the braces: the `if (main_span_ok_) { ... }` block now has an `else` (`kCodeMismatch`), and the outer `if (irq_blocked && lift::main_has(pc)) { ... }` has an `else`. Task 6 replaces this function entirely.

- [ ] **Step 7: Implement `write_coverage`**

In `system.cpp`, after `void System::dump_overlay_hist() { ... }`:

```cpp
bool System::write_coverage(const std::string& path) {
    if (!cov_on_) { log("[coverage] census disabled: SCD_LIFT_VERIFY replays translated code in Musashi\n"); return false; }
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { log("[coverage] cannot write %s\n", path.c_str()); return false; }
    std::string s = cov_.to_tsv(frames_, translate_);
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    log("[coverage] %s: not_translated %llu steps/%zu pcs, code_mismatch %llu, irq_entry %llu, hle_stub %llu, unsupported %llu, interpreter %llu\n",
        path.c_str(), (unsigned long long)cov_.steps(Fallback::kNotTranslated), cov_.distinct_pcs(Fallback::kNotTranslated),
        (unsigned long long)cov_.steps(Fallback::kCodeMismatch), (unsigned long long)cov_.steps(Fallback::kIrqEntry),
        (unsigned long long)cov_.steps(Fallback::kHleStub), (unsigned long long)cov_.steps(Fallback::kUnsupported),
        (unsigned long long)cov_.steps(Fallback::kInterpreter));
    return true;
}
```

- [ ] **Step 8: Write it from `scd_headless` on both exits**

In `engine/src/frontend/headless.cpp`, after the `const int profile_from = ...` line (before the frame loop), add:

```cpp
    const char* coverage_path = std::getenv("SCD_COVERAGE");   // ADR-003 fallback census, written on every exit (watchdog trips included)
    auto write_census = [&] { if (coverage_path) sys.write_coverage(coverage_path); };
```

Call `write_census();` directly before `write_failure_bundle(sys, out, trace, v);` in the watchdog branch, and directly before the final `sys.shutdown();` at the end of `main`.

- [ ] **Step 9: Run the tests**

Run:
```bash
cmake --build engine/build-interp -j --target scd_headless
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release -DSNATCHER_TRANSLATE_DIR=$PWD/extracted/code && cmake --build engine/build -j
.venv/bin/python tools/lift/test_coverage.py
```
Expected: 3 tests pass (`OK`).

- [ ] **Step 10: Check that nothing changed in emulation**

Run:
```bash
engine/build/lift_diff 8
tools/test_intro_skip.sh engine/build/scd_headless
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless engine/build-interp/scd_headless --frames 7700
```
Expected:
- `lift_diff` reports 0 failing cases.
- The intro-skip test passes.
- `run.py` exits 0. RAM-only differences are reported but pass, as documented in `docs/PLAYTEST.md`.

- [ ] **Step 11: Add to `test-recomp` and commit**

In the `test-recomp` task, add after `"engine/build-interp/coverage_test",`:

```toml
  "SCD_HEADLESS_INTERP=engine/build-interp/scd_headless python tools/lift/test_coverage.py",
```

```bash
git add engine/src/scd/system.h engine/src/scd/system.cpp engine/src/frontend/headless.cpp tools/lift/test_coverage.py mise.toml
git commit -m "recomp M1: SCD_COVERAGE fallback census per CPU, image and cause (ADR-003)"
```

---

### Task 3: Identify and dump overlays at `$28000`

**Files:**
- Modify: `engine/src/scd/system.h`
- Modify: `engine/src/scd/system.cpp` (move `crc32_of` out of the `#ifdef SNATCHER_TRANSLATED` block at line 424; add `note_overlay_exec`; hook it in `trace_pc` and `run_sub_translated`)
- Modify: `tools/lift/test_coverage.py`

**Interfaces:**
- Consumes: `ovl_image_`, `cov_on_`, `prg_ovl_gen_` (bumped on every CDC DMA into PRG RAM above `$28000`, `system.cpp:1050`).
- Produces:
  - `void System::note_overlay_exec()`
  - Env `SCD_DUMP_OVERLAYS=<dir>`, which writes `<dir>/ovl_<crc %08x>.bin`. The file is PRG RAM `$28000..$80000` (0x58000 bytes) captured at the first instruction executed there after a load. `<crc>` is the CRC32 (zlib polynomial) of its first 0x800 bytes and is the census image tag.
- Why the first 2 KB: the area above the overlay holds leftovers from earlier loads, so a CRC of the whole window would give the same code a different identity depending on history. The runtime's per-range CRC catches an overlay that shares its first 2 KB with a different one (Review Focus).

- [ ] **Step 1: Write the failing test**

Add to `TestInterpreterCensus` in `tools/lift/test_coverage.py`:

```python
    def test_overlay_dump_is_keyed_by_its_first_2k(self):
        d = OUT / "ovl"
        d.mkdir(parents=True, exist_ok=True)
        for p in d.glob("ovl_*.bin"):
            p.unlink()
        f = OUT / "ovl.tsv"
        run(INTERP, 1500, SCD_COVERAGE=str(f), SCD_DUMP_OVERLAYS=str(d))   # the intro overlay runs after the Konami logo
        dumps = sorted(d.glob("ovl_*.bin"))
        self.assertTrue(dumps, "no overlay dumped")
        for p in dumps:
            data = p.read_bytes()
            self.assertEqual(len(data), 0x58000)
            self.assertEqual(p.stem, "ovl_%08x" % zlib.crc32(data[:0x800]))
        images = {x[1] for x in rows(f) if x[0] == "sub" and int(x[3], 16) >= 0x28000}
        self.assertTrue(images, "no census rows tagged with an overlay image")
        self.assertLessEqual(images, {p.stem[4:] for p in dumps})
        self.assertNotIn("00000000", images)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `.venv/bin/python tools/lift/test_coverage.py -k overlay`
Expected: FAIL with `no overlay dumped`.

- [ ] **Step 3: Make `crc32_of` available to all builds**

In `system.cpp`, move the whole `static uint32_t crc32_of(const uint8_t* p, size_t n) { ... }` function from inside the `#ifdef SNATCHER_TRANSLATED` block (line 424) to just above that block's `#ifdef` (line 395). It is currently unused, so nothing else changes.

- [ ] **Step 4: Add the members and the snapshot**

In `system.h`, private section, next to `ovl_image_`:

```cpp
    void note_overlay_exec();                         // first execution at $28000 after a load: tag (and with SCD_DUMP_OVERLAYS dump) the image
    std::string ovl_dump_dir_;
    bool ovl_track_ = false;                          // census or overlay dump active
    uint32_t ovl_snap_gen_ = 0xFFFFFFFF;
```

In `System::init`, after the `cov_on_` line:

```cpp
    if (const char* d = std::getenv("SCD_DUMP_OVERLAYS")) ovl_dump_dir_ = d;
    ovl_track_ = cov_on_ || !ovl_dump_dir_.empty();
```

In `system.cpp`, after `write_coverage`:

```cpp
void System::note_overlay_exec() {
    if (ovl_snap_gen_ == prg_ovl_gen_) return;
    ovl_snap_gen_ = prg_ovl_gen_;
    ovl_image_ = crc32_of(prg_ram_ + 0x28000, 0x800);
    if (ovl_dump_dir_.empty()) return;
    char name[32];
    std::snprintf(name, sizeof name, "/ovl_%08x.bin", ovl_image_);
    std::string path = ovl_dump_dir_ + name;
    if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return; }
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(prg_ram_ + 0x28000, 1, 0x80000 - 0x28000, f);
        std::fclose(f);
        log("[overlay] dumped %s (load generation %u, frame %llu)\n", path.c_str(), prg_ovl_gen_, (unsigned long long)frames_);
    }
}
```

- [ ] **Step 5: Hook both execution paths**

In `trace_pc`, replace the census block from Task 2 with:

```cpp
    if (ovl_track_) {
        uint32_t p = pc & 0xFFFFFF;
        bool ovl = cur_ == kSub && p >= 0x28000 && p < 0x80000;
        if (ovl) note_overlay_exec();
        if (cov_on_) {
            Fallback why = fallback_why_;
            if (why == Fallback::kNotTranslated && (hle_.count(p) || hle_.count(p - 2))) why = Fallback::kHleStub;   // trap word, or the rts after it
            cov_.note(int(cur_), ovl ? ovl_image_ : 0, why, p);
        }
    }
```

In `run_sub_translated`, directly after `bool irq_blocked = ...;`, add:

```cpp
        if (ovl_track_ && pc >= 0x28000 && pc < 0x80000) note_overlay_exec();   // before translated overlay code can modify its own image
```

- [ ] **Step 6: Run the tests**

Run: `cmake --build engine/build-interp -j --target scd_headless && cmake --build engine/build -j && .venv/bin/python tools/lift/test_coverage.py`
Expected: 4 tests pass.

Then compare the new dump with the existing intro overlay:

```bash
.venv/bin/python -c "import glob,zlib; o=open('extracted/code/ovl_909.bin','rb').read(); print('ovl_909 key %08x' % zlib.crc32(o[:0x800])); print(glob.glob('work/cov_test/ovl/*.bin'))"
```

Expected: one dump's name contains the `ovl_909` key. If none matches, the overlay changes its first 2 KB between first execution and frame 1500. Record that in Task 7's journal entry. It does not block this plan, because the generic overlay program gets its own seeds.

- [ ] **Step 7: Commit**

```bash
git add engine/src/scd/system.h engine/src/scd/system.cpp tools/lift/test_coverage.py
git commit -m "recomp M1: identify overlays by their first 2 KB, SCD_DUMP_OVERLAYS (ADR-003)"
```

---

### Task 4: Coverage report, seed files and the gate

**Files:**
- Create: `tools/lift/coverage_report.py`
- Create: `tools/lift/test_coverage_report.py`
- Modify: `mise.toml` (`test-recomp`)

**Interfaces:**
- Consumes: the census TSV format (Task 1) and overlay image tags (Task 3).
- Produces:
  - CLI `coverage_report.py CENSUS.tsv... [--code-dir extracted/code] [--seeds-out DIR] [--check]`
  - Functions `read_census(path)`, `code_sizes(code_dir)`, `program_for(cpu, image, pc, sizes) -> str | None`, `merge(paths) -> Counter`, `write_seeds(total, sizes, out_dir) -> dict`, `report(total, sizes) -> (text, failures)`, `main(argv) -> int`
  - Program names: `main` (`$FF0000` + IP size), `mainram` (`$FFE100..$FFE3D0`), `sub` (SP at `$6000` and SUBCODE at `$D400`), `ovl_<image %08x>` (Sub, image != 0).
  - Seed file `<DIR>/<program>.txt`: a `#` comment line, then one lowercase 6-digit hex PC per line, sorted, merged (union) with any existing file. Seeds come from `interpreter` and `not_translated` rows.
  - `--check` exits 1 when any `not_translated` or `code_mismatch` row exists, mapped or not.

- [ ] **Step 1: Write the failing tests**

`tools/lift/test_coverage_report.py`:

```python
"""Unit tests for coverage_report.py (no disc needed)."""
import io, os, pathlib, sys, tempfile, unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import coverage_report as cr

HDR = "# snatcher-coverage v1 frames=100 translated=1\n"


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        code = self.dir / "code"
        code.mkdir()
        for name, size in (("scd_main_ip.bin", 0x6000), ("scd_sub_sp.bin", 0x1800), ("scd_subcode.bin", 0xD000)):
            (code / name).write_bytes(bytes(size))
        self.code = str(code)
        self.sizes = cr.code_sizes(self.code)

    def tearDown(self):
        self.tmp.cleanup()

    def census(self, name, body):
        p = self.dir / name
        p.write_text(HDR + body)
        return str(p)


class TestProgramFor(Base):
    def test_mapping(self):
        f = lambda cpu, image, pc: cr.program_for(cpu, image, pc, self.sizes)
        self.assertEqual(f("main", 0, 0xFF0584), "main")
        self.assertEqual(f("main", 0, 0xFFE100), "mainram")
        self.assertEqual(f("main", 0, 0xFFE3CE), "mainram")
        self.assertIsNone(f("main", 0, 0xFFE3D0))
        self.assertIsNone(f("main", 0, 0x00036C))
        self.assertEqual(f("sub", 0, 0x602A), "sub")
        self.assertEqual(f("sub", 0, 0xD5CC), "sub")
        self.assertIsNone(f("sub", 0, 0x5F22))
        self.assertEqual(f("sub", 0x1A2B3C4D, 0x28010), "ovl_1a2b3c4d")


class TestMergeAndSeeds(Base):
    def test_merge_sums_counts(self):
        a = self.census("a.tsv", "sub\t00000000\tnot_translated\t00d5cc\t2\n")
        b = self.census("b.tsv", "sub\t00000000\tnot_translated\t00d5cc\t3\n")
        self.assertEqual(cr.merge([a, b])[("sub", 0, "not_translated", 0xD5CC)], 5)

    def test_seeds_only_from_interpreter_and_not_translated(self):
        a = self.census("a.tsv",
                        "main\t00000000\tirq_entry\tff0584\t1\n"
                        "main\t00000000\tinterpreter\tff0600\t1\n"
                        "main\t00000000\thle_stub\t00036c\t1\n"
                        "sub\t00000000\tnot_translated\t00d5cc\t1\n"
                        "sub\t1a2b3c4d\tnot_translated\t028010\t1\n"
                        "sub\t00000000\tnot_translated\t005f22\t1\n")
        seeds = self.dir / "seeds"
        cr.write_seeds(cr.merge([a]), self.sizes, str(seeds))
        self.assertEqual(sorted(p.name for p in seeds.iterdir()), ["main.txt", "ovl_1a2b3c4d.txt", "sub.txt"])
        lines = lambda n: [l for l in (seeds / n).read_text().splitlines() if not l.startswith("#")]
        self.assertEqual(lines("main.txt"), ["ff0600"])
        self.assertEqual(lines("sub.txt"), ["00d5cc"])
        self.assertEqual(lines("ovl_1a2b3c4d.txt"), ["028010"])

    def test_seeds_accumulate_across_runs(self):
        seeds = self.dir / "seeds"
        cr.write_seeds(cr.merge([self.census("a.tsv", "sub\t00000000\tinterpreter\t00d5cc\t1\n")]), self.sizes, str(seeds))
        cr.write_seeds(cr.merge([self.census("b.tsv", "sub\t00000000\tinterpreter\t00d400\t1\n")]), self.sizes, str(seeds))
        body = [l for l in (seeds / "sub.txt").read_text().splitlines() if not l.startswith("#")]
        self.assertEqual(body, ["00d400", "00d5cc"])


class TestCheck(Base):
    def run_main(self, *args):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = cr.main(list(args))
        return rc, buf.getvalue()

    def test_check_passes_with_only_allowed_causes(self):
        a = self.census("a.tsv",
                        "main\t00000000\tirq_entry\tff0584\t9\n"
                        "main\t00000000\thle_stub\t00036c\t9\n"
                        "sub\t00000000\tunsupported\t00d600\t9\n")
        rc, out = self.run_main(a, "--code-dir", self.code, "--check")
        self.assertEqual(rc, 0, out)

    def test_check_fails_on_not_translated_and_names_unmapped(self):
        a = self.census("a.tsv", "sub\t00000000\tnot_translated\t016000\t4\n"
                                 "main\t00000000\tcode_mismatch\tffe100\t1\n")
        rc, out = self.run_main(a, "--code-dir", self.code, "--check")
        self.assertEqual(rc, 1)
        self.assertIn("unmapped:sub", out)
        self.assertIn("mainram", out)
        self.assertIn("FAIL", out)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run them to verify they fail**

Run: `.venv/bin/python tools/lift/test_coverage_report.py`
Expected: `ModuleNotFoundError: No module named 'coverage_report'`.

- [ ] **Step 3: Write the tool**

`tools/lift/coverage_report.py`:

```python
#!/usr/bin/env python3
"""Merge fallback censuses (SCD_COVERAGE files) into a report and translator seed files (ADR-003 M1).
Usage: coverage_report.py CENSUS.tsv... [--code-dir extracted/code] [--seeds-out DIR] [--check]
Seeds are disc-derived addresses: keep them under extracted/, never commit them."""
import argparse, collections, os, sys

GATE_REASONS = ('not_translated', 'code_mismatch')
SEED_REASONS = ('interpreter', 'not_translated')
MAINRAM = (0xFFE100, 0xFFE3D0)   # SP $7300..$75CF, copied by the Main CPU to $FFE100 (JOURNAL)


def read_census(path):
    rows = []
    with open(path) as f:
        for line in f:
            if not line.strip() or line.startswith('#'):
                continue
            cpu, image, reason, pc, count = line.rstrip('\n').split('\t')
            rows.append((cpu, int(image, 16), reason, int(pc, 16), int(count)))
    return rows


def code_sizes(code_dir):
    def size(name):
        p = os.path.join(code_dir, name)
        return os.path.getsize(p) if os.path.exists(p) else 0
    return {'ip': size('scd_main_ip.bin'), 'sp': size('scd_sub_sp.bin'), 'subcode': size('scd_subcode.bin')}


def program_for(cpu, image, pc, sizes):
    """The translator program (gen_program.py name) that owns pc, or None when no program covers that address."""
    if cpu == 'main':
        if 0xFF0000 <= pc < 0xFF0000 + sizes['ip']:
            return 'main'
        if MAINRAM[0] <= pc < MAINRAM[1]:
            return 'mainram'
        return None
    if image:
        return 'ovl_%08x' % image
    if 0x6000 <= pc < 0x6000 + sizes['sp'] or 0xD400 <= pc < 0xD400 + sizes['subcode']:
        return 'sub'
    return None


def merge(paths):
    total = collections.Counter()
    for p in paths:
        for cpu, image, reason, pc, count in read_census(p):
            total[(cpu, image, reason, pc)] += count
    return total


def write_seeds(total, sizes, out_dir):
    by_prog = collections.defaultdict(set)
    for (cpu, image, reason, pc) in total:
        prog = program_for(cpu, image, pc, sizes) if reason in SEED_REASONS else None
        if prog:
            by_prog[prog].add(pc)
    os.makedirs(out_dir, exist_ok=True)
    for prog, pcs in by_prog.items():
        path = os.path.join(out_dir, prog + '.txt')
        if os.path.exists(path):
            with open(path) as f:
                pcs |= {int(l, 16) for l in f if l.strip() and not l.startswith('#')}
        with open(path, 'w') as f:
            f.write('# executed PCs from coverage censuses (disc-derived, do not commit)\n')
            f.writelines('%06x\n' % pc for pc in sorted(pcs))
    return {prog: len(pcs) for prog, pcs in by_prog.items()}


def report(total, sizes):
    steps, pcs = collections.Counter(), collections.Counter()
    for (cpu, image, reason, pc), n in total.items():
        steps[reason] += n
        pcs[reason] += 1
    lines = ['%-15s %14s %10s' % ('cause', 'steps', 'pcs')]
    lines += ['%-15s %14d %10d' % (r, steps[r], pcs[r]) for r in sorted(steps)]
    gate = collections.Counter()
    for (cpu, image, reason, pc) in total:
        if reason in GATE_REASONS:
            gate[(reason, program_for(cpu, image, pc, sizes) or 'unmapped:' + cpu)] += 1
    if gate:
        lines += ['', 'gate failures (distinct PCs):']
        lines += ['  %-15s %-20s %d' % (r, prog, n) for (r, prog), n in sorted(gate.items())]
        worst = sorted(((n, k) for k, n in total.items() if k[2] in GATE_REASONS), reverse=True)[:20]
        lines += ['', 'top gate PCs (cpu image cause pc steps):']
        lines += ['  %s %08x %s %06x %d' % (cpu, image, reason, pc, n) for n, (cpu, image, reason, pc) in worst]
    return '\n'.join(lines), sum(gate.values())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('census', nargs='+')
    ap.add_argument('--code-dir', default='extracted/code')
    ap.add_argument('--seeds-out', help='write/extend <dir>/<program>.txt seed files for gen_program.py')
    ap.add_argument('--check', action='store_true', help='exit 1 if any step ran in the interpreter for want of translation')
    a = ap.parse_args(argv)
    sizes = code_sizes(a.code_dir)
    total = merge(a.census)
    text, failures = report(total, sizes)
    print(text)
    if a.seeds_out:
        for prog, n in sorted(write_seeds(total, sizes, a.seeds_out).items()):
            print('seeds %-20s %d' % (prog, n))
    if a.check and failures:
        print('FAIL: %d PCs ran in the interpreter with cause not_translated or code_mismatch' % failures)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 4: Run the tests**

Run: `.venv/bin/python tools/lift/test_coverage_report.py`
Expected: 6 tests, `OK`.

- [ ] **Step 5: Try it on the real census from Task 3 and commit**

Run: `.venv/bin/python tools/lift/coverage_report.py work/cov_test/tr.tsv --code-dir extracted/code`
Expected: a cause table. `not_translated` is likely non-zero for `mainram` and `unmapped:*`. Those are what Tasks 5 and 6 address.

Add `"python tools/lift/test_coverage_report.py",` to `test-recomp` after the `coverage_test` line.

```bash
git add tools/lift/coverage_report.py tools/lift/test_coverage_report.py mise.toml
git commit -m "recomp M1: coverage report, seed files and --check gate (ADR-003)"
```

---

### Task 5: Translator: seeds, region windows, `mainram` and generic overlay programs

**Files:**
- Modify: `tools/lift/gen_program.py`
- Create: `tools/lift/test_gen_program.py`
- Modify: `mise.toml` (`test-recomp`)

**Interfaces:**
- Consumes: seed files `<code dir>/seeds/<program>.txt` (Task 4), overlay dumps `<code dir>/overlays/ovl_<crc>.bin` (Task 3).
- Produces:
  - `gen_program.py <out> <code dir> mainram` writes `mainram_translated.cpp` (bytes `0x1300..0x15D0` of `scd_sub_sp.bin` at `$FFE100`, entry `$FFE100`). Symbols: `mainram_has`, `mainram_run`, `kMainramRanges`, `kMainramRangeCount`, `kMainramCrc`.
  - `gen_program.py <out> <code dir> ovl:overlays/ovl_<crc>.bin` writes `ovl_<crc>_translated.cpp`, based at `$28000` and seeded only from `seeds/ovl_<crc>.txt`. Symbols: `ovl_<crc>_has`, `ovl_<crc>_run`, `kOvl_<crc>Ranges`, `kOvl_<crc>RangeCount`, `kOvl_<crc>Crc`.
  - `gen_program.py <out> <code dir> ovl_index` writes `overlays_index.cpp`, defining `lift::kOverlays[]` (type `lift::Program`, Task 6) and `lift::kOverlayCount`.
  - Every program adds the in-range PCs from `seeds/<program>.txt` to its entry points.

- [ ] **Step 1: Confirm the `$FFE100` copy matches SP `$7300..$75CF`**

The journal says the Main copies SP `$7300..$75CF` to `$FFE100`. Check that the bytes are identical (no relocation):

```bash
mkdir -p work/mainram_check
SCD_NO_TRANSLATE=1 SCD_DUMP_RAM=1 engine/build-interp/scd_headless "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" --frames 1500 --out work/mainram_check
.venv/bin/python -c "a=open('work/mainram_check/main_ram.bin','rb').read()[0xE100:0xE3D0]; b=open('extracted/code/scd_sub_sp.bin','rb').read()[0x1300:0x15D0]; print('match' if a==b else 'DIFF %d bytes' % sum(x!=y for x,y in zip(a,b)))"
```

Expected: `match`. If it prints `DIFF`, stop and report the difference to the user before continuing, because the `mainram` window below assumes an unrelocated copy.

- [ ] **Step 2: Write the failing tests**

`tools/lift/test_gen_program.py`:

```python
"""Tests for gen_program.py seeds, region windows, mainram and overlay programs (synthetic code, no disc needed)."""
import pathlib, subprocess, sys, tempfile, unittest

GEN = pathlib.Path(__file__).with_name("gen_program.py")
MOVEQ1_RTS = bytes.fromhex("70014E75")
MOVEQ2_RTS = bytes.fromhex("70024E75")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.code = pathlib.Path(self.tmp.name) / "code"
        self.out = pathlib.Path(self.tmp.name) / "out"
        self.code.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def gen(self, prog):
        r = subprocess.run([sys.executable, str(GEN), str(self.out), str(self.code), prog], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def seeds(self, prog, *pcs):
        d = self.code / "seeds"
        d.mkdir(exist_ok=True)
        (d / (prog + ".txt")).write_text("# test\n" + "".join("%06x\n" % pc for pc in pcs))


class TestMainRam(Base):
    def setUp(self):
        super().setUp()
        sp = bytearray(0x1800)
        sp[0x1300:0x1304] = MOVEQ1_RTS          # $FFE100: entry
        sp[0x1310:0x1314] = MOVEQ2_RTS          # $FFE110: reachable only through a seed
        (self.code / "scd_sub_sp.bin").write_bytes(bytes(sp))

    def test_window_is_based_at_ffe100(self):
        self.gen("mainram")
        cpp = (self.out / "mainram_translated.cpp").read_text()
        self.assertIn("bool mainram_has(uint32_t pc)", cpp)
        self.assertIn("case 0xFFE100:", cpp)
        self.assertNotIn("case 0xFFE110:", cpp)
        self.assertIn("kMainramRanges", cpp)

    def test_seed_adds_entry(self):
        self.seeds("mainram", 0xFFE110)
        self.gen("mainram")
        self.assertIn("case 0xFFE110:", (self.out / "mainram_translated.cpp").read_text())

    def test_out_of_range_seed_is_ignored(self):
        self.seeds("mainram", 0x500000, 0xFFE3D0)
        self.gen("mainram")
        cpp = (self.out / "mainram_translated.cpp").read_text()
        self.assertNotIn("case 0x500000:", cpp)
        self.assertNotIn("case 0xFFE3D0:", cpp)


class TestOverlays(Base):
    def write_overlay(self, name, body):
        d = self.code / "overlays"
        d.mkdir(exist_ok=True)
        img = bytearray(0x1000)
        img[0:len(body)] = body
        (d / (name + ".bin")).write_bytes(bytes(img))

    def test_overlay_program_from_seeds_only(self):
        self.write_overlay("ovl_deadbeef", MOVEQ1_RTS)
        self.seeds("ovl_deadbeef", 0x28000)
        self.gen("ovl:overlays/ovl_deadbeef.bin")
        cpp = (self.out / "ovl_deadbeef_translated.cpp").read_text()
        self.assertIn("bool ovl_deadbeef_has(uint32_t pc)", cpp)
        self.assertIn("case 0x28000:", cpp)
        self.assertIn("kOvl_deadbeefRanges", cpp)

    def test_same_pc_different_bytes_gets_different_range_crc(self):
        self.write_overlay("ovl_00000001", MOVEQ1_RTS)
        self.write_overlay("ovl_00000002", MOVEQ2_RTS)
        self.seeds("ovl_00000001", 0x28000)
        self.seeds("ovl_00000002", 0x28000)
        self.gen("ovl:overlays/ovl_00000001.bin")
        self.gen("ovl:overlays/ovl_00000002.bin")
        # the instruction-range line ({lo, hi, crc}); the span line for the whole file starts {0x28000, 0x29000, ...}
        crc = lambda n: [l for l in (self.out / (n + "_translated.cpp")).read_text().splitlines() if l.strip().startswith("{0x28000, 0x28004,")]
        self.assertEqual(len(crc("ovl_00000001")), 1)
        self.assertNotEqual(crc("ovl_00000001"), crc("ovl_00000002"))

    def test_index_lists_every_overlay(self):
        self.write_overlay("ovl_deadbeef", MOVEQ1_RTS)
        self.gen("ovl_index")
        cpp = (self.out / "overlays_index.cpp").read_text()
        self.assertIn("{ovl_deadbeef_has, ovl_deadbeef_run, kOvl_deadbeefRanges, &kOvl_deadbeefRangeCount, &kOvl_deadbeefCrc}", cpp)
        self.assertIn("kOverlayCount = 1;", cpp)

    def test_index_without_overlays(self):
        self.gen("ovl_index")
        self.assertIn("kOverlayCount = 0;", (self.out / "overlays_index.cpp").read_text())


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: Run them to verify they fail**

Run: `.venv/bin/python tools/lift/test_gen_program.py`
Expected: failures with `KeyError: 'mainram'`, `KeyError: 'ovl:overlays/...'` and `KeyError: 'ovl_index'` in the subprocess stderr.

- [ ] **Step 4: Implement the changes in `gen_program.py`**

Add `glob` to the imports: `import glob, os, sys, zlib, re`.

Update the docstring usage line:

```python
Usage: tools/lift/gen_program.py <outdir> [code dir] [sub|main|mainram|ovl909|ovl:<file under code dir>|ovl_index]
```

Add to `PROGRAMS`, after `'main'`:

```python
    # SP $7300..$75CF, copied by the Main CPU to $FFE100 and run there (BURAM wrapper etc., JOURNAL). Region = (file, base, offset, length).
    'mainram': dict(regions=[('scd_sub_sp.bin', 0xFFE100, 0x1300, 0x2D0)], entries=[0xFFE100], vectors=False, mask=0xFFFFFF, lo_code=0xFFE100),
```

Add these functions after `static_target`:

```python
def read_seeds(codedir, prog, in_code):
    """Executed PCs from coverage censuses (coverage_report.py --seeds-out): extra entry points. Out-of-range PCs are ignored."""
    path = os.path.join(codedir, 'seeds', prog + '.txt')
    if not os.path.exists(path):
        return set()
    with open(path) as f:
        pcs = {int(l, 16) for l in f if l.strip() and not l.startswith('#')}
    return {pc for pc in pcs if in_code(pc)}


def write_overlay_index(outdir, codedir):
    """overlays_index.cpp: the runtime's table of overlay programs, one per <code dir>/overlays/ovl_*.bin."""
    names = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(codedir, 'overlays', 'ovl_*.bin')))
    out = ['// GENERATED from the user\'s disc by tools/lift/gen_program.py. Do not commit or distribute.', '#include "cpu68k.h"', 'namespace lift {']
    for n in names:
        k = n.capitalize()
        out.append('bool %s_has(uint32_t pc); Exit %s_run(Cpu& c, uint32_t pc, int32_t& budget);' % (n, n))
        out.append('extern const Span k%sRanges[]; extern const int k%sRangeCount; extern const uint32_t k%sCrc;' % (k, k, k))
    out.append('extern const Program kOverlays[] = {')
    out += ['    {%s_has, %s_run, k%sRanges, &k%sRangeCount, &k%sCrc},' % (n, n, n.capitalize(), n.capitalize(), n.capitalize()) for n in names]
    if not names:
        out.append('    {nullptr, nullptr, nullptr, nullptr, nullptr},')
    out += ['};', 'extern const int kOverlayCount = %d;' % len(names), '}  // namespace lift']
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, 'overlays_index.cpp'), 'w').write('\n'.join(out) + '\n')
    print('overlay index: %d overlays' % len(names))
```

In `main()`, replace:

```python
    prog = sys.argv[3] if len(sys.argv) > 3 else 'sub'
    cfg = PROGRAMS[prog]
```

with:

```python
    prog = sys.argv[3] if len(sys.argv) > 3 else 'sub'
    if prog == 'ovl_index':
        return write_overlay_index(outdir, codedir)
    if prog.startswith('ovl:'):   # an overlay dump (SCD_DUMP_OVERLAYS); entry points come from its seed file
        rel = prog[4:]
        prog = os.path.splitext(os.path.basename(rel))[0]
        cfg = dict(regions=[(rel, 0x28000)], entries=[], vectors=False, mask=0xFFFFFF, lo_code=0x28000)
    else:
        cfg = PROGRAMS[prog]
```

Replace the region loading loop:

```python
    for f, base in REGIONS:
        d = open(os.path.join(codedir, f), 'rb').read()
```

with:

```python
    for f, base, *window in REGIONS:
        d = open(os.path.join(codedir, f), 'rb').read()
        if window:
            d = d[window[0]:window[0] + window[1]]
```

Directly before `insns = {}`, add:

```python
    entries |= read_seeds(codedir, prog, in_code)
```

- [ ] **Step 5: Run the tests**

Run: `.venv/bin/python tools/lift/test_gen_program.py`
Expected: 7 tests, `OK`.

Then check that existing programs are unchanged when no seed files exist:

```bash
mkdir -p work/gen_before work/gen_after
# the committed generator, run from a copy next to its helpers (no git stash: the stash is shared between worktrees)
git show HEAD:tools/lift/gen_program.py > tools/lift/gen_program_head.py
.venv/bin/python tools/lift/gen_program_head.py work/gen_before extracted/code sub && mv tools/lift/gen_program_head.py work/gen_before/
.venv/bin/python tools/lift/gen_program.py work/gen_after extracted/code sub
cmp work/gen_before/sub_translated.cpp work/gen_after/sub_translated.cpp && echo identical
```

Expected: `identical`, provided `extracted/code/seeds/` does not exist yet.

- [ ] **Step 6: Commit**

Add `"python tools/lift/test_gen_program.py",` to `test-recomp`.

```bash
git add tools/lift/gen_program.py tools/lift/test_gen_program.py mise.toml
git commit -m "recomp M1: translator seeds, region windows, mainram and overlay programs (ADR-003)"
```

---

### Task 6: Runtime: overlay registry, Main RAM code, per-range checks on Main

**Files:**
- Modify: `engine/src/lift/cpu68k.h` (add `Program`)
- Modify: `engine/src/scd/system.h`
- Modify: `engine/src/scd/system.cpp` (declarations at lines 13-37, `TEntry`/`entry_code_ok`/`sub_entries` at lines 430-473, `run_main_translated`, `main_write8` at line 746)
- Modify: `engine/CMakeLists.txt` (translation block)

**Interfaces:**
- Consumes: the generated symbols from Task 5.
- Produces:
  - `struct lift::Program { has, run, ranges, const int* nranges, const uint32_t* crc }`
  - `TEntry::mask`
  - `main_entries()`
  - `System::main_code_gen_`, bumped on Main writes to `$FFE100..$FFE3D0`
  - CMake picks up `extracted/code/overlays/ovl_*.bin` and `extracted/code/seeds/*.txt` through `CONFIGURE_DEPENDS` globs.

- [ ] **Step 1: Add `Program` to `cpu68k.h`**

After `struct Span { uint32_t lo, hi, crc; };`:

```cpp
// A translated program as the runtime's registry sees it (overlays_index.cpp, generated by gen_program.py ovl_index).
// Counts are pointers so the table can be constant-initialized across translation units.
struct Program {
    bool (*has)(uint32_t);
    Exit (*run)(Cpu&, uint32_t, int32_t&);
    const Span* ranges;
    const int* nranges;
    const uint32_t* crc;
};
```

- [ ] **Step 2: Rework the CMake translation block**

In `engine/CMakeLists.txt`, replace everything from `if(SNATCHER_TRANSLATE_DIR)` to its matching `endif()` with:

```cmake
if(SNATCHER_TRANSLATE_DIR)
  find_package(Python3 REQUIRED)
  set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  set(LIFT ${CMAKE_CURRENT_SOURCE_DIR}/../tools/lift)
  # Seed files and overlay dumps come from `mise run census` (ADR-003); new files are picked up on the next build.
  file(GLOB LIFT_SEEDS CONFIGURE_DEPENDS ${SNATCHER_TRANSLATE_DIR}/seeds/*.txt)
  file(GLOB LIFT_OVERLAYS CONFIGURE_DEPENDS ${SNATCHER_TRANSLATE_DIR}/overlays/ovl_*.bin)
  set(LIFT_DEPS ${LIFT}/gen_program.py ${LIFT}/m68k_decode.py ${LIFT}/emit.py ${LIFT_SEEDS})
  function(lift_program prog out)
    add_custom_command(OUTPUT ${GEN_DIR}/${out}
      COMMAND ${Python3_EXECUTABLE} ${LIFT}/gen_program.py ${GEN_DIR} ${SNATCHER_TRANSLATE_DIR} ${prog}
      DEPENDS ${LIFT_DEPS} ${ARGN})
    target_sources(scdcore PRIVATE ${GEN_DIR}/${out})
  endfunction()
  lift_program(sub sub_translated.cpp)
  if(EXISTS ${SNATCHER_TRANSLATE_DIR}/ovl_909.bin)
    lift_program(ovl909 ovl909_translated.cpp ${SNATCHER_TRANSLATE_DIR}/ovl_909.bin)
    target_compile_definitions(scdcore PUBLIC SNATCHER_TRANSLATED_OVL909=1)
  endif()
  if(EXISTS ${SNATCHER_TRANSLATE_DIR}/scd_main_ip.bin)
    lift_program(main main_translated.cpp)
    lift_program(mainram mainram_translated.cpp)
    target_compile_definitions(scdcore PUBLIC SNATCHER_TRANSLATED_MAIN=1)
  endif()
  foreach(bin ${LIFT_OVERLAYS})
    get_filename_component(stem ${bin} NAME_WE)
    lift_program(ovl:overlays/${stem}.bin ${stem}_translated.cpp ${bin})
  endforeach()
  lift_program(ovl_index overlays_index.cpp ${LIFT_OVERLAYS})
  target_compile_definitions(scdcore PUBLIC SNATCHER_TRANSLATED=1)
endif()
```

- [ ] **Step 3: Update the declarations at the top of `system.cpp`**

Replace the `#ifdef SNATCHER_TRANSLATED_MAIN ... #endif` block inside `namespace lift` (lines 30-35) with:

```cpp
#ifdef SNATCHER_TRANSLATED_MAIN
bool main_has(uint32_t pc);
Exit main_run(Cpu& c, uint32_t pc, int32_t& budget);
extern const Span kMainRanges[];
extern const int kMainRangeCount;
extern const uint32_t kMainCrc;
bool mainram_has(uint32_t pc);
Exit mainram_run(Cpu& c, uint32_t pc, int32_t& budget);
extern const Span kMainramRanges[];
extern const int kMainramRangeCount;
extern const uint32_t kMainramCrc;
#endif
extern const Program kOverlays[];
extern const int kOverlayCount;
```

- [ ] **Step 4: Generalise `TEntry`, `entry_code_ok` and `sub_entries`**

In `struct TEntry`, after `bool overlay;` add:

```cpp
    uint32_t mask = 0xFFFFFF;     // CPU address -> offset into the memory array checked (Main RAM: 0xFFFF)
```

In `entry_code_ok`, replace:

```cpp
    if (st == 0) st = crc_range(mem, e.ranges[e.last].lo, e.ranges[e.last].hi) == e.ranges[e.last].crc ? 1 : 2;
```

with:

```cpp
    const lift::Span& r = e.ranges[e.last];
    if (st == 0) st = crc_range(mem, r.lo & e.mask, ((r.hi - 1) & e.mask) + 1) == r.crc ? 1 : 2;
```

Replace `sub_entries()` with:

```cpp
static std::vector<TEntry>& sub_entries() {
    static std::vector<TEntry> e = [] {
        std::vector<TEntry> v = {
            {lift::sub_has, lift::sub_run, lift::kSubRanges, lift::kSubRangeCount, lift::kSubCrc, false},
#ifdef SNATCHER_TRANSLATED_OVL909
            {lift::ovl909_has, lift::ovl909_run, lift::kOvl909Ranges, lift::kOvl909RangeCount, lift::kOvl909Crc, true},
#endif
        };
        for (int i = 0; i < lift::kOverlayCount; ++i) {   // overlay dumps from the census (ADR-003), first CRC match wins
            const lift::Program& p = lift::kOverlays[i];
            v.push_back({p.has, p.run, p.ranges, *p.nranges, *p.crc, true});
        }
        return v;
    }();
    return e;
}
```

- [ ] **Step 5: Rewrite `run_main_translated` on top of `TEntry`**

In `system.h`, delete `bool main_span_checked_ = false, main_span_ok_ = false;` and add in the same place:

```cpp
    uint32_t main_code_gen_ = 0;   // bumped by Main writes to the $FFE100 code window: its ranges are re-checked
```

In `system.cpp`, replace the whole `#ifdef SNATCHER_TRANSLATED_MAIN ... int System::run_main_translated(int cycles) { ... } #endif` with:

```cpp
#ifdef SNATCHER_TRANSLATED_MAIN
static std::vector<TEntry>& main_entries() {
    static std::vector<TEntry> e = {
        {lift::main_has, lift::main_run, lift::kMainRanges, lift::kMainRangeCount, lift::kMainCrc, false, 0xFFFF},
        {lift::mainram_has, lift::mainram_run, lift::kMainramRanges, lift::kMainramRangeCount, lift::kMainramCrc, true, 0xFFFF},
    };
    return e;
}

int System::run_main_translated(int cycles) {
    static MainBusAdapter bus(*this);
    lift::Cpu c;
    c.bus = &bus;
    int32_t budget = cycles;
    main_budget_ = &budget;
    while (budget > 0) {
        uint32_t pc = reg_get(M68K_REG_PC) & 0xFFFFFF;
        uint32_t sr = reg_get(M68K_REG_SR);
        bool irq_blocked = main_irq_level() <= int((sr >> 8) & 7);
        TEntry* te = nullptr;
        bool claimed = false;
        if (irq_blocked) {
            for (auto& e : main_entries()) {
                if (!e.has(pc)) continue;
                claimed = true;
                if (entry_code_ok(e, main_ram_, pc, e.overlay ? main_code_gen_ : 0)) { te = &e; break; }
            }
        }
        fallback_why_ = !irq_blocked ? Fallback::kIrqEntry : claimed ? Fallback::kCodeMismatch : Fallback::kNotTranslated;
        if (te) {
            for (int i = 0; i < 8; ++i) { c.d[i] = reg_get(M68K_REG_D0 + i); c.a[i] = i < 7 ? reg_get(M68K_REG_A0 + i) : reg_get(M68K_REG_SP); }
            c.set_ccr(sr & 0x1F);
            c.sr_sys = uint16_t(sr & 0xA700);
            lift::Exit ex = te->run(c, pc, budget);
            for (int i = 0; i < 8; ++i) { reg_set(M68K_REG_D0 + i, c.d[i]); if (i < 7) reg_set(M68K_REG_A0 + i, c.a[i]); }
            reg_set(M68K_REG_SP, c.a[7]);
            reg_set(M68K_REG_SR, c.sr_sys | c.ccr());
            reg_set(M68K_REG_PC, ex.pc);
            if (ex.kind == lift::Exit::Budget) break;
            if (halted_) break;
            if (ex.kind == lift::Exit::NotTranslated) continue;   // re-dispatch: the IP calls into $FFE100 and back
            fallback_why_ = Fallback::kUnsupported;
        }
        ++fallback_steps_;
        int used = m68k_execute(1);
        budget -= used > 0 ? used : 4;
        if (halted_) break;
    }
    main_budget_ = nullptr;
    return cycles - budget;
}
#endif
```

This removes the old one-time whole-IP CRC check and its `[translate] Main code differs` log line. A mismatch now shows up as `code_mismatch` in the census. Also remove the now-unused `kMainSpans` / `kMainSpanCount` declarations if nothing else references them (`grep -n kMainSpan engine/src`).

- [ ] **Step 6: Invalidate `$FFE100` checks on writes**

In `main_write8`, replace:

```cpp
    if (a >= 0xE00000) main_ram_[a & 0xFFFF] = uint8_t(v);
```

with:

```cpp
    if (a >= 0xE00000) {
        main_ram_[a & 0xFFFF] = uint8_t(v);
        if ((a & 0xFFFF) - 0xE100u < 0x2D0u) ++main_code_gen_;   // the $FFE100 code (mainram program) changed: re-check its CRC ranges
    }
```

- [ ] **Step 7: Build and run every guard**

Run:
```bash
cmake --build engine/build-interp -j
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release -DSNATCHER_TRANSLATE_DIR=$PWD/extracted/code && cmake --build engine/build -j
.venv/bin/python tools/lift/gen_tests.py engine/tests/lift_cases.inc 20000 && cmake --build engine/build --target lift_diff && engine/build/lift_diff 8
tools/test_intro_skip.sh engine/build/scd_headless
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless engine/build-interp/scd_headless --frames 7700
SCD_LIFT_VERIFY=1 engine/build/scd_headless "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" --frames 3000 --out work/verify 2>&1 | grep '\[lift-verify\]' | grep -v 'op=4e73' | head
.venv/bin/python tools/lift/test_coverage.py
```

Expected:
- `lift_diff` reports 0 failing cases, and the intro-skip test passes.
- `run.py` exits 0.
- The `SCD_LIFT_VERIFY` grep prints nothing. `op=4e73` is `rte`, the known artifact.
- `test_coverage.py` passes.
- If `ovl_909` differs from the title screen check, also run the MAME title-screen comparison described in `CLAUDE.md` ("Oracle workflow").

- [ ] **Step 8: Commit**

```bash
git add engine/src/lift/cpu68k.h engine/src/scd/system.h engine/src/scd/system.cpp engine/CMakeLists.txt
git commit -m "recomp M1: overlay registry, translated \$FFE100 Main code, per-range checks on Main (ADR-003)"
```

---

### Task 7: Census pipeline, the M1 gate, and documentation

**Files:**
- Create: `tools/lift/census.sh`
- Modify: `mise.toml` (tasks `census`, `coverage`)
- Modify: `docs/JOURNAL.md`, `docs/ADR-003-static-recompilation.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: everything above. `tools/playtest/run.py` takes `--headless`, `--frames` and `--bram`, and passes the environment through to `scd_headless`.
- Produces:
  - `tools/lift/census.sh <scd_headless> <out dir> <replay>...`
  - `mise run census`, which runs the interpreter build over the reference replays, dumps overlays and writes seeds
  - `mise run coverage`, which runs the translated build over the same replays and fails on gate causes

- [ ] **Step 1: Write the census script**

`tools/lift/census.sh`:

```bash
#!/usr/bin/env bash
# Run reference replays on one headless build and write a fallback census per replay (ADR-003 M1).
# usage: tools/lift/census.sh <scd_headless> <out dir> <replay>...
# Backup RAM comes from <replay>.bram when present. SCD_DUMP_OVERLAYS (if set) is passed through; CENSUS_FRAMES sets the run length.
set -euo pipefail
headless=$1; out=$2; shift 2
frames=${CENSUS_FRAMES:-60000}
mkdir -p "$out"
if [ -n "${SCD_DUMP_OVERLAYS:-}" ]; then mkdir -p "$SCD_DUMP_OVERLAYS"; fi
status=0
for replay in "$@"; do
  name=$(basename "$replay" .replay)
  args=(--headless "$headless" --frames "$frames")
  if [ -f "$replay.bram" ]; then args+=(--bram "$replay.bram"); fi
  echo "census: $name"
  if ! SCD_COVERAGE="$out/$name.tsv" .venv/bin/python tools/playtest/run.py "$replay" "${args[@]}"; then
    echo "census: $name did not finish cleanly; its census is still written" >&2
    status=1
  fi
done
exit $status
```

Run: `chmod +x tools/lift/census.sh && tools/lift/census.sh engine/build-interp/scd_headless work/census/smoke tools/playtest/replays/intro_skip.replay && head -3 work/census/smoke/intro_skip.tsv`
Expected: `census: intro_skip`, then a header line `# snatcher-coverage v1 frames=... translated=0` followed by `interpreter` rows.

- [ ] **Step 2: Add the mise tasks**

Append to `mise.toml`:

```toml
[tasks.census]
description = "ADR-003 M1: run the reference replays on the interpreter build, dump overlays and write translator seeds (extracted/code)"
run = [
  "cmake -S engine -B engine/build-interp -DCMAKE_BUILD_TYPE=Release",
  "cmake --build engine/build-interp -j --target scd_headless",
  "SCD_DUMP_OVERLAYS=extracted/code/overlays tools/lift/census.sh engine/build-interp/scd_headless work/census/interp tools/playtest/replays/*.replay ${CENSUS_REPLAYS:-}",
  "python tools/lift/coverage_report.py work/census/interp/*.tsv --code-dir extracted/code --seeds-out extracted/code/seeds",
]

[tasks.coverage]
description = "ADR-003 M1 gate: run the reference replays on the translated build; fails if code ran in the interpreter for want of translation"
run = [
  "mise run build",
  "tools/lift/census.sh engine/build/scd_headless work/census/translated tools/playtest/replays/*.replay ${CENSUS_REPLAYS:-}",
  "python tools/lift/coverage_report.py work/census/translated/*.tsv --code-dir extracted/code --seeds-out extracted/code/seeds --check",
]
```

`CENSUS_REPLAYS` names your own recorded sessions under `work/` (with their `.bram`), for example `CENSUS_REPLAYS="work/replays/act1.replay work/replays/act2.replay"`. The `coverage` task also writes seeds, so a failing gate followed by `mise run build` and `mise run coverage` converges.

- [ ] **Step 3: Run the loop on the committed replay**

Run:
```bash
mise run census
mise run coverage
```

Expected: `census` prints a seeds line for `main`, `mainram`, `sub` and at least one `ovl_<crc>`. `coverage` ends with exit 0, or with a `FAIL` and a gate table.

If it fails:
1. Run `mise run coverage` a second time. The seeds written by the first failing run feed the rebuild.
2. If `unmapped:*` rows remain, they are code at addresses no program covers. Examples: a Sub overlay loaded somewhere other than `$28000`, or code run from Word RAM. List them from the report's "top gate PCs" and **stop and report them to the user**. Each needs a new program definition, which is a design decision, not a mechanical fix.
3. If `code_mismatch` rows remain, record the PCs and the image tag in the journal and report them to the user. Their cause (self-modifying code, or an overlay sharing its first 2 KB with another) decides the fix.

- [ ] **Step 4: Record reference replays (user action)**

The intro-skip replay only reaches the first in-game screen. Ask the user to record sessions with `engine/build/snatcher "<cue>" --record-input work/replays/<name>.replay` covering:
- each act
- a shooting section
- save and load
- the options and sound test

Then run `CENSUS_REPLAYS="work/replays/*.replay" mise run census` and `CENSUS_REPLAYS="work/replays/*.replay" mise run coverage`.

M1 is complete when `coverage` passes over the full set. Until those replays exist, report M1 as "gate passes on intro_skip only".

- [ ] **Step 5: Document**

Append to `docs/JOURNAL.md` (fill in the numbers the report printed):

```markdown
## 2026-10-05: Recomp M1 coverage census (ADR-003)
- `SCD_COVERAGE=<file>` records every Musashi step per CPU, code image and cause; `SCD_DUMP_OVERLAYS=<dir>` dumps each image executed at `$28000`, keyed by CRC32 of its first 2 KB.
- The `$FFE100` Main code equals SP `$7300..$75CF` byte for byte (checked at frame 1500) and is translated as program `mainram`.
- intro_skip, translated build, before seeds: not_translated <N> steps / <P> pcs (<programs>), code_mismatch <N>, irq_entry <N>, hle_stub <N>, unsupported <N>.
- After `mise run census` + `mise run coverage`: <results>. Overlay images seen: <list of ovl_ keys>, ovl_909 key <key> (matches / differs).
```

Add a `## Progress` section to `docs/ADR-003-static-recompilation.md` with the same numbers and the replay set they were measured on. Change `Status: proposed` to `Status: accepted` only if the user has accepted the ADR.

In `CLAUDE.md`:
- Add `SCD_COVERAGE=<file>` and `SCD_DUMP_OVERLAYS=<dir>` to the env var list in "Build, run, test".
- Add one sentence after the `ovl_909.bin` paragraph: "`mise run census` (interpreter build over the reference replays) dumps the other overlays to `extracted/code/overlays/` and writes translator seeds to `extracted/code/seeds/`; `mise run coverage` is the ADR-003 M1 gate."

- [ ] **Step 6: Final checks and commit**

Run:
```bash
mise run test-recomp
engine/build/lift_diff 8
git status --short
```

Expected:
- All tests pass and `lift_diff` reports 0 failing cases.
- `git status` lists only the files below. Nothing under `extracted/`, `work/` or `engine/build*` appears. Those paths are gitignored. If any do appear, stop.

```bash
git add tools/lift/census.sh mise.toml docs/JOURNAL.md docs/ADR-003-static-recompilation.md CLAUDE.md
git commit -m "recomp M1: census pipeline, coverage gate, docs (ADR-003)"
```
