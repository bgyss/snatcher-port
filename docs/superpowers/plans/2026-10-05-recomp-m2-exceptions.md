# Recomp M2: Exceptions in Host Code Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take interrupts in host code, translate `rte`, `move to sr`, `andi/ori/eori #,sr`, `stop`, `trap`, `chk` and divide-by-zero, and remove the `irq_blocked` guard, so that on the reference replays the only Musashi steps left are HLE stubs and host-built BIOS glue.

**Architecture:**
- `engine/src/lift/cpu68k.h` gains the 68000 exception model, checked against Musashi by `lift_diff`: an inactive stack pointer (`other_sp`), `set_sr` (stack swap on S changes; reports a now-deliverable interrupt), `exception_entry` (trap, chk, divide by zero) and `interrupt_entry` (autovectored interrupt). Two new exits: `Exit::Irq` (an SR write unmasked a pending level) and `Exit::Stop`.
- The dispatch loops in `System::run_sub_translated` and `run_main_translated` take interrupts in host code. They do it exactly where M1's loop let Musashi take them: at the top of each dispatch step, followed by the handler's first instruction before the next check. They also take them where Musashi's `m68ki_set_sr` does, inside an SR write (`Exit::Irq`). A translated `stop` parks the CPU until an interrupt arrives.
- The decoder emits the new instructions. `gen_program.py` charges an exception's total cycles. `gen_tests.py` adds synthetic encodings so `lift_diff` covers every new instruction whatever the disc contains. `coverage_report.py` reports host-built BIOS glue as `hle_stub` and gains an `m2` gate.

**Tech Stack:** C++20 (CMake, `scdcore`, `scd_headless`, `lift_diff`, Musashi), Python 3 (`.venv/bin/python`, stdlib `unittest`), mise, bash.

**Spec:** `docs/ADR-003-static-recompilation.md` (milestone M2). Background: `docs/ADR-002-build-time-translator.md`, `docs/JOURNAL.md`, and the M1 plan `docs/superpowers/plans/2026-10-05-recomp-m1-coverage.md` with the census numbers in the ADR's Progress section.

## Global Constraints

- Never commit disc images, BIOS dumps, extracted data, listings, MAME captures, generated translated C++, `engine/tests/lift_cases.inc`, coverage censuses, seed files, overlay dumps or probe output. They live in `extracted/`, `work/` or `engine/build*/`, which are all gitignored.
- Stage files by explicit path only. Never use `git add -A`, `git add .` or `git commit -a`, because untracked symlinks `extracted/` and `sega-cd-eng/` exist in this worktree.
- Build on macOS with Apple clang: `CC=/usr/bin/clang CXX=/usr/bin/clang++`. A cached CMake dir configured with another compiler must be replaced, not reconfigured.
- The safety hook blocks `rm -rf`. Use a fresh build directory or `mv`. In Python tests use `Path.unlink()`.
- Python tools run with `.venv/bin/python`.
- Bit-identity: `engine/build/lift_diff 8` must report `failing cases 0` and `failing interrupt entries 0` after every task that touches the translator or `cpu68k.h`.
- Bit-identity: after every task that changes generated code or the runtime, the translated build must match the **parent reference build** `work/parent-src/engine/build/scd_headless` (Task 1, source `c98c81f`) at every checkpoint, RAM included (`tools/playtest/run.py ... --strict-ram`), on `tools/playtest/replays/intro_skip.replay` over 60000 frames. The only exception is the branch Task 5 spells out.
- Build the parent reference once in Task 1 and never rebuild it. Do not write seed files before Task 7: no `--seeds-out`, no `mise run census`, no `mise run coverage`. The parent and the M2 build must translate the same PCs.
- Return addresses stay 24-bit in M2. Frames pushed in host code carry the PC that M1's Musashi step pushed. At the top of a dispatch step that is Musashi's PC register (24-bit after a translated exit). Inside an SR write it is the translated next PC (`rte`: the popped value, unmasked). Translated `jsr`/`bsr` keep pushing 24-bit constants (`tools/lift/emit.py:11`, `:13`), where the interpreter pushes `$FFFFxxxx` on Main. `rts`/`rte` mask the popped value before dispatch, so control flow is unaffected, and the strict-RAM gate is against the parent. Matching the interpreter's stack bytes is M3 work, together with cycle accounting.
- Cycle accounting: an instruction that raises an exception costs exactly `lift::exc_cycles(vector)` in total, and an interrupt entry costs `lift::exc_cycles(24 + level)` = 44, both as in Musashi. Everything else keeps base-only cycles (`m68ki_cycles[0][op]`, `tools/lift/gen_program.py:218`). Dynamic cycle adjustments are M3.
- A privileged instruction executed in user mode leaves translated code with `Exit::Unsupported`, and Musashi takes the privilege violation. Task 1 checks that the game never runs in user mode.
- Level 7 (NMI, edge-triggered in Musashi) is not modelled in host delivery. Nothing in the Mega CD raises it.
- `cpu68k.h` must not reference Musashi symbols (M4 removes Musashi). Generated code may keep using `m68ki_cycles`.
- In headless runs, build `--press` lists in bash (zsh does not word-split an unquoted `$VAR`).
- No linter exists.

## Review Focus

- **An interrupt raised while translated code runs**, for example by the CPU's own I/O write (the Sub unmasking a pending level through `$FF8033`, Main enabling V-INT in the VDP), or a pending level unmasked by an SR write. Expected: it is taken where Musashi takes it, either inside the SR write that unmasks it or at the top of the next dispatch step. Never mid-instruction, never lost, never taken twice. Tests: Task 5 `lift_diff` SR-write cases with a random pending level, Task 3 `loop_step` unit test, Task 3 and Task 5 strict-RAM runs.
- **Nested interrupts and level masking.** A higher level arrives inside a handler (Main's V-INT handler polls VBLANK inside a nested V-INT, JOURNAL 2026-10-02). Equal and lower levels stay blocked. After an entry, the handler's first instruction runs before the next check. Tests: Task 2 `irq_trials` (mask anywhere below the level, either mode, trace bit cleared), Task 3 `loop_step` (`force_one` suppresses a second delivery).
- **`rte` restoring SR and PC**, including the supervisor bit, the USP/SSP swap when the popped SR leaves supervisor mode, a popped `$FFFFxxxx` PC, and `rte` in user mode (privilege violation, left to Musashi). Tests: Task 5 synthetic and disc `rte` cases with random S, random popped SR and a full SR plus inactive-SP compare. `SCD_LIFT_VERIFY` (Task 3) now compares SR and the inactive SP too.
- **Trap vectors.** `trap #0..#15` uses vector 32+n with the next instruction's PC in the frame. Taken from user mode it switches to the SSP. It costs 34 cycles in total. An empty vector table entry is read as is (only interrupts fall back to vector 15). Tests: Task 4 synthetic `trap` cases with random vector tables and a cycle compare, and the Task 4 `gen_program` harness test.
- **`stop` waking on an interrupt.** With a level above the new mask pending, the interrupt is taken immediately. Otherwise the CPU idles for the rest of the slice and wakes at the next step where an interrupt is deliverable, and the frame PC is the instruction after `stop`. Tests: Task 4 `stop` cases (`4E722000`, `4E722700`, `4E720000`) with random pending levels, Task 3 `loop_step` `kIdle`/`kDeliver` while stopped.
- **Main CPU as well as Sub.** Main's vectors go through its own map: `$70` returns the Gate Array H-INT vector (`hint_vec_`), so the handler starts at `$FFFF0xxx` in the IP; `$78` points at the RAM jump slot `$FFFFFD06`. The handler entry must go through `pick_entry` with the per-range CRC (Main RAM `mask 0xFFFF`, `main_code_gen_`), and VDP DMA stalls during the handler's first instruction must still be charged (`main_budget_`). Tests: Task 3 `test_interrupts_enter_translated_handlers` (no `irq_entry` or `code_mismatch` rows, Main Musashi steps only at RAM jump slots) and the strict-RAM run. Task 6 checks every remaining step is glue.

## Files

| File | Responsibility | Tasks |
|---|---|---|
| `engine/src/lift/cpu68k.h` | 68000 exception model for translated code | 2 |
| `engine/tests/lift_diff.cpp` | translated vs Musashi: instructions and interrupt entry | 2, 4 |
| `tools/lift/gen_tests.py` | `lift_cases.inc`: disc encodings, synthetic encodings, setup class | 2, 4, 5 |
| `engine/src/scd/translate_entry.h` | `loop_step`: the dispatch loop's interrupt/idle/run decision | 3 |
| `engine/tests/translate_entry_test.cpp` | `loop_step` unit test | 3 |
| `engine/src/scd/system.h`, `system.cpp` | dispatch loops, `lift_load`/`lift_store`/`take_irq`, `stopped_`, verify mode | 3 |
| `tools/lift/test_coverage.py` | end-to-end census tests | 3, 6 |
| `tools/lift/m68k_decode.py`, `emit.py` | decoder: exceptions (Task 4), SR writes (Task 5) | 4, 5 |
| `tools/lift/gen_program.py` | descent past exceptions, exception cycles | 4 |
| `tools/lift/test_m68k_decode.py` (new) | decoder output tests | 4, 5 |
| `tools/lift/test_gen_program.py` | `trap` program test | 4 |
| `tools/lift/coverage_report.py`, `test_coverage_report.py` | host glue as `hle_stub`, `--gate m2` | 6 |
| `mise.toml`, `docs/JOURNAL.md`, `docs/ADR-003-static-recompilation.md`, `CLAUDE.md` | tasks, facts, progress | 1, 4, 7 |

---

### Task 1: Investigation: parent reference build, deferred interrupts, user mode

Facts later tasks branch on. Only `docs/JOURNAL.md` is committed. The probe is applied, run and reverted.

**Files:**
- Temporary (reverted in Step 8): `engine/src/lift/cpu68k.h`, `tools/lift/m68k_decode.py` (lines 369-375 and 457-458), `tools/lift/emit.py` (line 15), `engine/src/scd/system.h`, `engine/src/scd/system.cpp` (`init` at line 151, `trace_pc` at line 359, the two dispatch loops at lines 479 and 558)
- Modify: `docs/JOURNAL.md` (append)

**Interfaces:**
- Produces:
  - `work/parent-src/engine/build/scd_headless`: the M1 translated build at `c98c81f`, the strict-RAM reference for Tasks 3 to 7. Never rebuilt.
  - Journal facts F1 to F5, which later tasks branch on:
    - **F1** = number of translated SR writes that left a pending level above the new mask (an interrupt the parent defers but Musashi takes at once). Branch **D1** in Task 5.
    - **F2** = number of user-mode observations (S = 0). Branch **D2** below.
    - **F3** = number of dispatch-loop steps where `*_irq_level()` differs from `cpu_irq_[c]`. Branch **D3** in Task 3.
    - **F4**: the exception and privileged opcodes present in the translated programs.
    - **F5**: the first visible divergence frame, parent vs interpreter, on intro_skip.

- [ ] **Step 1: Build the parent reference**

Run:
```bash
mkdir -p work/parent-src
git archive c98c81f engine tools | tar -x -C work/parent-src
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S work/parent-src/engine -B work/parent-src/engine/build -DCMAKE_BUILD_TYPE=Release -DSNATCHER_TRANSLATE_DIR=$PWD/extracted/code
cmake --build work/parent-src/engine/build -j --target scd_headless
```
Expected: `work/parent-src/engine/build/scd_headless` exists. The configure step prints nothing about missing `ovl_909.bin`.

- [ ] **Step 2: Check the reference against the current build**

The worktree's code is still `c98c81f`, so the two builds must agree byte for byte.

Run:
```bash
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release -DSNATCHER_TRANSLATE_DIR=$PWD/extracted/code
cmake --build engine/build -j --target scd_headless
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless work/parent-src/engine/build/scd_headless --frames 60000 --strict-ram --work work/m2/t1_self
```
Expected: `ok: builds match at every checkpoint`. If not, stop: the reference is unusable, so report the output.

- [ ] **Step 3: Record the parent vs the interpreter (F5)**

Run:
```bash
cmake --build engine/build-interp -j --target scd_headless
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless work/parent-src/engine/build/scd_headless --compare-headless engine/build-interp/scd_headless --frames 60000 --work work/m2/t1_interp
```
Expected: `DIVERGED (visible): ...` (JOURNAL 2026-10-05 already reports a visible divergence around frame 300). Note the first visible divergence frame as F5.

- [ ] **Step 4: Static scan of the translated programs (F4)**

Run:
```bash
.venv/bin/python - <<'EOF'
import re, glob, collections
def cls(op):
    if op == 0x4E73: return 'rte'
    if op == 0x4E72: return 'stop'
    if 0x4E40 <= op <= 0x4E4F: return 'trap'
    if 0x4E60 <= op <= 0x4E6F: return 'move_usp'
    if op & 0xFFC0 == 0x46C0: return 'move_to_sr'
    if op in (0x007C, 0x027C, 0x0A7C): return {0x007C: 'ori_sr', 0x027C: 'andi_sr', 0x0A7C: 'eori_sr'}[op]
    if op & 0xF1C0 == 0x4180: return 'chk'
    if op & 0xF1C0 in (0x80C0, 0x81C0): return 'div'
    return None
for f in sorted(glob.glob('engine/build/generated/*_translated.cpp')):
    s = open(f).read(); n = collections.Counter()
    for m in re.finditer(r'case 0x[0-9A-F]+: \{ //\s*\n\s*budget -= m68ki_cycles\[0\]\[(0x[0-9A-F]+)\];', s):
        c = cls(int(m.group(1), 16))
        if c: n[c] += 1
    print(f.split('/')[-1], dict(n))
EOF
```
Expected (from the plan author's run on the M1 build): no `trap`, `stop` or `move_usp` in any program. `chk` appears only in the overlay programs (`ovl909` 5, `ovl_936869c7` 2, `ovl_a7b6f72d` 1), at addresses that only the `ovl909` linear scan decoded, so it is data, not code. Record the table as F4.

- [ ] **Step 5: Apply the temporary probe**

In `engine/src/lift/cpu68k.h`, directly after the closing `};` of `struct Cpu`, add:

```cpp
// TEMPORARY (M2 Task 1 probe): called after every translated SR write.
inline void (*sr_probe_hook)(uint16_t sr, uint32_t pc) = nullptr;
inline void sr_probe(const Cpu& c, uint32_t pc) { if (sr_probe_hook) sr_probe_hook(uint16_t(c.sr_sys | c.ccr()), pc); }
```

In `tools/lift/m68k_decode.py`, in `_group0`, replace

```python
        else: raise Unsupported('imm to ccr kind')
        return
```

with

```python
        else: raise Unsupported('imm to ccr kind')
        if B == 2: code.append('lift::sr_probe(c, %s);' % hx(ctx.pc))
        return
```

In `_group4`, replace the `move to sr` line

```python
        ea = Operand(ctx, code, mode, reg, 2); v = ctx.t(); code.append('uint32_t %s = %s; c.set_ccr(%s); c.sr_sys = %s & 0xA700;' % (v, ea.read(), v, v)); return
```

with

```python
        ea = Operand(ctx, code, mode, reg, 2); v = ctx.t(); code.append('uint32_t %s = %s; c.set_ccr(%s); c.sr_sys = %s & 0xA700;' % (v, ea.read(), v, v)); code.append('lift::sr_probe(c, %s);' % hx(pc)); return
```

In `tools/lift/emit.py`, replace line 15 with

```python
    if f[0] == 'rte': return ['{ uint32_t f = c.bus->r16(c.a[7]); c.a[7] += 2; npc = c.bus->r32(c.a[7]); c.a[7] += 4; c.set_ccr(f); c.sr_sys = f & 0xA700; lift::sr_probe(c, %s); }' % hx(ins.pc)]
```

In `engine/src/scd/system.h`, at the end of the public section (after `int int_ack(int level);`), add:

```cpp
    // TEMPORARY (M2 Task 1 probe).
    void irq_probe(uint16_t sr, uint32_t pc, bool translated_sr_write);
    void irq_probe_report();
    bool probe_on_ = false;
    uint64_t probe_sr_writes_ = 0, probe_user_ = 0, probe_level_diff_ = 0;
    std::map<std::pair<int, uint32_t>, uint64_t> probe_deferred_;
```

In `engine/src/scd/system.cpp`, in `System::init`, after `trace_audio_ = std::getenv("SCD_TRACE_AUDIO") != nullptr;`, add:

```cpp
    probe_on_ = std::getenv("SCD_IRQ_PROBE") != nullptr;   // TEMPORARY (M2 Task 1)
#ifdef SNATCHER_TRANSLATED
    if (probe_on_) lift::sr_probe_hook = [](uint16_t sr, uint32_t pc) { System::instance().irq_probe(sr, pc, true); };
#endif
    if (probe_on_) std::atexit([] { System::instance().irq_probe_report(); });
```

Directly before `void System::trace_pc(uint32_t pc) {`, add:

```cpp
void System::irq_probe(uint16_t sr, uint32_t pc, bool translated_sr_write) {   // TEMPORARY (M2 Task 1)
    if (!(sr & 0x2000)) ++probe_user_;
    if (!translated_sr_write) return;
    ++probe_sr_writes_;
    if (cpu_irq_[cur_] > int((sr >> 8) & 7)) ++probe_deferred_[{int(cur_), pc & 0xFFFFFF}];
}

void System::irq_probe_report() {
    uint64_t deferred = 0;
    for (auto& kv : probe_deferred_) deferred += kv.second;
    log("[irq-probe] translated SR writes %llu, unmasking a pending level %llu, user-mode observations %llu, loop-top level disagreements %llu\n",
        (unsigned long long)probe_sr_writes_, (unsigned long long)deferred, (unsigned long long)probe_user_, (unsigned long long)probe_level_diff_);
    for (auto& kv : probe_deferred_)
        log("[irq-probe] deferred %s %06x %llu\n", kv.first.first ? "sub" : "main", kv.first.second, (unsigned long long)kv.second);
}
```

At the start of `trace_pc`, add `if (probe_on_) irq_probe(uint16_t(reg_get(M68K_REG_SR)), pc, false);`.

In `run_sub_translated`, after `uint32_t sr = reg_get(M68K_REG_SR);`, add:

```cpp
        if (probe_on_) { if (sub_irq_level() != cpu_irq_[kSub]) ++probe_level_diff_; irq_probe(uint16_t(sr), pc, false); }
```

In `run_main_translated`, after `uint32_t sr = reg_get(M68K_REG_SR);`, add:

```cpp
        if (probe_on_) { if (main_irq_level() != cpu_irq_[kMain]) ++probe_level_diff_; irq_probe(uint16_t(sr), pc, false); }
```

- [ ] **Step 6: Build the probe and run it**

Run:
```bash
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S engine -B engine/build-probe -DCMAKE_BUILD_TYPE=Release -DSNATCHER_TRANSLATE_DIR=$PWD/extracted/code
cmake --build engine/build-probe -j --target scd_headless
mkdir -p work/m2/probe
SCD_IRQ_PROBE=1 engine/build-probe/scd_headless "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" --frames 60000 --out work/m2/probe --replay tools/playtest/replays/intro_skip.replay 2>&1 | grep '\[irq-probe\]' | tee work/m2/probe/report.txt
```
Expected: a summary line, then zero or more `deferred` lines. Read the values off the summary line: F1 is "unmasking a pending level", F2 is "user-mode observations", F3 is "loop-top level disagreements". Keep the `deferred` lines: they name the PCs (Main critical-section ends such as `move (sp)+,sr`, and `andi #$F8FF,sr`).

- [ ] **Step 7: Run the probe on the interpreter path too (F2 for Musashi-only code)**

Run:
```bash
SCD_NO_TRANSLATE=1 SCD_IRQ_PROBE=1 engine/build-probe/scd_headless "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" --frames 60000 --out work/m2/probe --replay tools/playtest/replays/intro_skip.replay 2>&1 | grep '\[irq-probe\] translated'
```
Expected: `translated SR writes 0`. The user-mode count covers every instruction Musashi executed. Add it to F2.

- [ ] **Step 8: Revert the probe**

Run:
```bash
git checkout -- engine/src/lift/cpu68k.h tools/lift/m68k_decode.py tools/lift/emit.py engine/src/scd/system.h engine/src/scd/system.cpp
git status --short
```
Expected: `git status` shows nothing tracked as modified. `engine/build-probe/` is ignored. Leave it in place, or `mv` it under `work/`.

- [ ] **Step 9: Write the facts**

Append to `docs/JOURNAL.md` (fill in the values from Steps 3, 4, 6 and 7, and use the date of the run):

```markdown
## 2026-10-05: Recomp M2 investigation (ADR-003)
- Parent reference for M2: the M1 translated build at `c98c81f` (`work/parent-src`, not committed). It matches the M1 HEAD build at every intro_skip checkpoint (60000 frames, RAM included). Versus the interpreter build its first visible divergence is at frame <F5>.
- Exception and privileged opcodes in the translated programs (static): <F4 table>. No `trap`, `stop` or `move usp`; `chk` only in overlay bytes decoded by the `ovl909` linear scan (data).
- Interrupt probe, intro_skip, 60000 frames, translated build: <N> translated SR writes; <F1> of them left a pending level above the new mask (Musashi would take the interrupt inside the instruction; the M1 loop takes it at the next dispatch step). PCs: <deferred lines>.
- User mode: <F2> observations (translated SR writes, dispatch-loop tops and every Musashi step, translated and interpreter runs). Level disagreements (`*_irq_level()` vs the IPL Musashi sees): <F3>.
- The handlers' first instructions after an interrupt entry (M1 census `irq_entry` PCs) are `move sr,-(sp)` (`$40E7`, no dynamic cycles) at `$FF0CCE`, `$FF0D6E`, `$FF0EFA` (H-INT handlers vectored through `$A12006`), and the RAM jump slots `$FFFD06` (Main V-INT), `$5F7C` and `$5F82` (Sub levels 2 and 3).
```

- [ ] **Step 10: Decide the branches and commit**

- **D2:** if F2 > 0, stop and report to the user before Task 2. The plan leaves privileged instructions in user mode to Musashi, and a game that runs in user mode needs a translated privilege-violation path instead.
- **D3** (consumed by Task 3): if F3 > 0, Task 3's strict-RAM gate may fail at those steps. Task 3 says what to do.
- **D1** (consumed by Task 5): F1 = 0 or F1 > 0.

Run:
```bash
git add docs/JOURNAL.md
git commit -m "recomp M2: investigation facts (parent reference, deferred interrupts, user mode) (ADR-003)"
```

---

### Task 2: The exception model in `cpu68k.h`, checked by `lift_diff`

**Files:**
- Modify: `engine/src/lift/cpu68k.h` (`struct Exit` at lines 19-24, `struct Cpu` at lines 40-48; new helpers after `cond()`, which ends at line 107)
- Modify: `engine/tests/lift_diff.cpp` (whole file)
- Modify: `tools/lift/gen_tests.py` (case table, setup class, synthetic list)

**Interfaces:**
- Produces (all in `namespace lift`):
  - `Exit::Kind` gains `Irq` and `Stop`, appended after `NotTranslated`, so the existing kinds keep their numbers: `None 0, Unsupported 1, DivZero 2, Trap 3, Budget 4, NotTranslated 5, Irq 6, Stop 7`. `DivZero` and `Trap` are no longer produced after Task 4.
  - `Cpu` gains:
    - `uint32_t other_sp = 0;`: the inactive stack pointer (USP in supervisor mode, SSP in user mode)
    - `uint16_t exc_cycles = 0;`: set by `exception_entry` to the total cost of the instruction that raised it
    - `const int* ipl = nullptr;`: the pending interrupt level as the CPU sees it
  - `constexpr int exc_cycles(int vector)`: 38 for vector 5, 40 for 6, 44 for 15 and 24..31, 34 otherwise.
  - `bool set_sr(Cpu& c, uint32_t v)`: sets CCR and `sr_sys = v & 0xA700`, and swaps `a[7]`/`other_sp` when S changes. Returns `true` when `*ipl` is above the new mask.
  - `uint16_t enter_exception(Cpu& c)`: returns the old SR, enters supervisor mode (swapping to the SSP), clears trace.
  - `void push_frame(Cpu& c, uint32_t pc, uint16_t sr)`: pushes PC (long) then SR (word).
  - `uint32_t exception_entry(Cpu& c, int vector, uint32_t pc)`: frame, then the vector read. Sets `exc_cycles`. Returns the handler address.
  - `uint32_t interrupt_entry(Cpu& c, int level, uint32_t pc)`: raises the mask to `level`, reads vector 24+level (falling back to vector 15 when it is 0), then the frame. Returns the handler address. The caller acknowledges the interrupt and charges `exc_cycles(24 + level)`.
- `lift_cases.inc` (generated by `gen_tests.py`): `struct Case { uint32_t pc; int size; uint8_t bytes[12]; lift::Exit (*fn)(lift::Cpu&, uint32_t&); int kind; };`. `kind` is 0 for plain, 1 for an SR write, 2 for an instruction that may raise an exception.

- [ ] **Step 1: Give `gen_tests.py` the setup class and the synthetic list**

In `tools/lift/gen_tests.py`, replace the docstring and add the helpers after `BLOBS`:

```python
#!/usr/bin/env python3
"""Emit engine/tests/lift_cases.inc: one C++ function per distinct instruction found in the user's code blobs, plus synthetic
encodings of privileged and exception instructions (ADR-003 M2) that the disc code may lack.
Usage: tools/lift/gen_tests.py out.inc [max_cases]   (reads extracted/code/*.bin; output is derived from the disc: do not commit)"""
```

```python
# Generic 68000 encodings (not disc-derived), always tested whatever the disc contains (ADR-003 M2).
SYNTHETIC = []
SYN_PC = 0x00F000


def kind_of(ins):
    """lift_diff's setup class: 1 = SR write (either mode, a pending interrupt level), 2 = may raise an exception, 0 = plain."""
    return 1 if getattr(ins, 'sr_write', False) else 2 if ins.flow == ('exc',) else 0


def synthetic_cases():
    out = []
    for hexs in SYNTHETIC:
        raw = bytes.fromhex(hexs) + bytes(12)
        r16 = lambda a, d=raw: (d[a - SYN_PC] << 8 | d[a - SYN_PC + 1]) if 0 <= a - SYN_PC < len(d) - 1 else 0
        ins = decode(r16, SYN_PC)
        if ins.code and ins.code[0].startswith('/* unsupported'):
            sys.exit('synthetic case %s does not translate: %s' % (hexs, ins.code[0]))
        out.append((ins, raw[:ins.size]))
    return out
```

In `main()`, replace

```python
    random.Random(1).shuffle(cases); cases = cases[:cap]
```

with

```python
    random.Random(1).shuffle(cases); cases = cases[:cap] + synthetic_cases()
```

and replace the two lines that write the case table:

```python
        f.write('struct Case { uint32_t pc; int size; uint8_t bytes[12]; lift::Exit (*fn)(lift::Cpu&, uint32_t&); };\nstatic const Case kCases[] = {\n')
        for i, (ins, raw) in enumerate(cases):
            f.write('  {%s, %d, {%s}, t_%d},\n' % (hx(ins.pc), len(raw), ','.join(str(b) for b in raw), i))
```

with

```python
        f.write('struct Case { uint32_t pc; int size; uint8_t bytes[12]; lift::Exit (*fn)(lift::Cpu&, uint32_t&); int kind; };\nstatic const Case kCases[] = {\n')
        for i, (ins, raw) in enumerate(cases):
            f.write('  {%s, %d, {%s}, t_%d, %d},\n' % (hx(ins.pc), len(raw), ','.join(str(b) for b in raw), i, kind_of(ins)))
```

- [ ] **Step 2: Write the new `lift_diff.cpp` (the failing test)**

Replace `engine/tests/lift_diff.cpp` with:

```cpp
// Differential test (ADR-002, ADR-003 M2): every translated instruction (lift_cases.inc: the encodings in your disc's code,
// generated by tools/lift/gen_tests.py, plus synthetic privileged and exception encodings) against Musashi on random states,
// and interrupt entry in host code (lift::interrupt_entry) against Musashi taking the same interrupt.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "cpu68k.h"
extern "C" {
#include "m68k.h"
int scd_illg_handler(int) { return 0; }
int scd_int_ack(int) { return M68K_INT_ACK_AUTOVECTOR; }
void scd_instruction_hook(unsigned) {}
extern unsigned char m68ki_cycles[][0x10000];
extern const unsigned char m68ki_exception_cycle_table[][256];
}

static std::vector<uint8_t> g_mem;          // Musashi's image
struct FlatBus : lift::Bus {
    std::vector<uint8_t> m;
    uint8_t r8(uint32_t a) override { return m[a & 0xFFFFFF]; }
    uint16_t r16(uint32_t a) override { return uint16_t(m[a & 0xFFFFFF] << 8 | m[(a + 1) & 0xFFFFFF]); }
    void w8(uint32_t a, uint8_t v) override { m[a & 0xFFFFFF] = v; }
    void w16(uint32_t a, uint16_t v) override { m[a & 0xFFFFFF] = v >> 8; m[(a + 1) & 0xFFFFFF] = uint8_t(v); }
};
extern "C" {
unsigned m68k_read_memory_8(unsigned a) { return g_mem[a & 0xFFFFFF]; }
unsigned m68k_read_memory_16(unsigned a) { return g_mem[a & 0xFFFFFF] << 8 | g_mem[(a + 1) & 0xFFFFFF]; }
unsigned m68k_read_memory_32(unsigned a) { return m68k_read_memory_16(a) << 16 | m68k_read_memory_16(a + 2); }
unsigned m68k_read_disassembler_8(unsigned a) { return m68k_read_memory_8(a); }
unsigned m68k_read_disassembler_16(unsigned a) { return m68k_read_memory_16(a); }
unsigned m68k_read_disassembler_32(unsigned a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned a, unsigned v) { g_mem[a & 0xFFFFFF] = uint8_t(v); }
void m68k_write_memory_16(unsigned a, unsigned v) { m68k_write_memory_8(a, v >> 8); m68k_write_memory_8(a + 1, v); }
void m68k_write_memory_32(unsigned a, unsigned v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }
}
#include "lift_cases.inc"

static uint64_t rng_s = 88172645463325252ull;
static uint32_t rnd() { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return uint32_t(rng_s >> 16); }

// Random bytes in 32 KB windows around each base, identical in both images.
static void fill(FlatBus& bus, const std::vector<uint32_t>& bases, uint64_t s) {
    for (uint32_t b : bases) {
        uint32_t base = (b - 0x4000) & 0xFFFFFF;
        for (uint32_t i = 0; i < 0x8000; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; bus.m[(base + i) & 0xFFFFFF] = uint8_t(s >> 24); }
    }
    std::memcpy(g_mem.data(), bus.m.data(), g_mem.size());
}

// Musashi's registers from a lift::Cpu: the inactive stack pointer goes to USP (supervisor) or ISP (user), as System::lift_store does.
static void musashi_set(const lift::Cpu& c, uint32_t pc) {
    for (int i = 0; i < 8; ++i) { m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), c.d[i]); if (i < 7) m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), c.a[i]); }
    m68k_set_reg(M68K_REG_SR, c.sr_sys | c.ccr());
    m68k_set_reg(M68K_REG_SP, c.a[7]);
    m68k_set_reg((c.sr_sys & 0x2000) ? M68K_REG_USP : M68K_REG_ISP, c.other_sp);
    m68k_set_reg(M68K_REG_PC, pc);
}

struct Checker {
    const char* label;
    int& fails;
    bool bad = false;
    void cmp(const char* what, uint32_t a, uint32_t b) {
        if (a == b) return;
        if (!bad && fails < 25) std::printf("%s: %s lift=%08X musashi=%08X\n", label, what, a, b);
        bad = true;
    }
    // full: the whole SR and the inactive stack pointer (cases that can change them); otherwise the CCR only.
    void regs(const lift::Cpu& c, uint32_t npc, bool full) {
        for (int i = 0; i < 8; ++i) {
            char nm[8];
            std::snprintf(nm, 8, "d%d", i); cmp(nm, c.d[i], m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i)));
            std::snprintf(nm, 8, "a%d", i); cmp(nm, c.a[i], m68k_get_reg(nullptr, i < 7 ? m68k_register_t(M68K_REG_A0 + i) : M68K_REG_SP));
        }
        if (full) {
            cmp("inactive sp", c.other_sp, m68k_get_reg(nullptr, (c.sr_sys & 0x2000) ? M68K_REG_USP : M68K_REG_ISP));
            cmp("sr", c.sr_sys | c.ccr(), m68k_get_reg(nullptr, M68K_REG_SR));
        } else {
            cmp("ccr", c.ccr(), m68k_get_reg(nullptr, M68K_REG_SR) & 0x1F);
        }
        cmp("pc", npc, m68k_get_reg(nullptr, M68K_REG_PC));
    }
    void memory(const FlatBus& bus, const std::vector<uint32_t>& bases) {
        for (uint32_t b : bases) {
            uint32_t base = (b - 0x4000) & 0xFFFFFF;
            for (uint32_t i = 0; i < 0x8000 && !bad; ++i) { uint32_t a = (base + i) & 0xFFFFFF; cmp("memory", bus.m[a], g_mem[a]); }
        }
    }
};

// Interrupt entry in host code: lift::interrupt_entry plus the handler's first instruction (a nop) against Musashi taking the
// same autovectored interrupt at the start of m68k_execute. Masks anywhere below the level (nested entries), either mode.
static int irq_trials(FlatBus& bus, int n, int& checked) {
    int fails = 0;
    for (int t = 0; t < n; ++t) {
        lift::Cpu c; c.bus = &bus;
        for (int i = 0; i < 8; ++i) { c.d[i] = rnd() ^ (rnd() << 16); c.a[i] = 0x100000 + (rnd() & 0xFFFFF) * 2; }
        c.a[7] = 0x800000 + (rnd() & 0xFFF) * 2;
        c.other_sp = 0x700000 + (rnd() & 0xFFF) * 2;
        const int level = 1 + int(rnd() % 6);
        const int mask = int(rnd() % unsigned(level));
        c.sr_sys = uint16_t(((rnd() & 1) ? 0x2000 : 0) | (mask << 8) | ((rnd() & 1) ? 0x8000 : 0));   // trace set sometimes: the entry clears it
        c.set_ccr(rnd() & 0x1F);
        const uint32_t pc = ((rnd() & 1) ? 0xFF000000u : 0u) | (0xF000 + (rnd() & 0x7FF) * 2);   // pushed as Musashi holds it, unmasked
        const bool uninit = t % 8 == 7;                                       // an empty vector: Musashi takes vector 15
        const std::vector<uint32_t> bases = {c.a[7], c.other_sp, 0, 0xE000, pc & 0xFFFFFF};
        fill(bus, bases, rnd() | 1);
        auto put = [&](uint32_t a, uint32_t v, int bytes) {
            for (int i = 0; i < bytes; ++i) { uint8_t b = uint8_t(v >> (8 * (bytes - 1 - i))); bus.m[a + i] = b; g_mem[a + i] = b; }
        };
        put(uint32_t(24 + level) * 4, uninit ? 0 : 0xE000, 4);
        put(15 * 4, 0xE000, 4);
        put(0xE000, 0x4E71, 2);                                               // nop
        m68k_pulse_reset(); m68k_execute(1);
        musashi_set(c, pc);
        m68k_set_irq(unsigned(level));
        const int used = m68k_execute(1);                                     // interrupt entry, then the nop
        const uint32_t npc = lift::interrupt_entry(c, level, pc) + 2;
        ++checked;
        char label[64];
        std::snprintf(label, sizeof label, "interrupt trial %d level %d mask %d", t, level, mask);
        Checker ck{label, fails};
        ck.regs(c, npc, true);
        ck.cmp("cycles", uint32_t(lift::exc_cycles(24 + level) + m68ki_cycles[0][0x4E71]), uint32_t(used));
        ck.memory(bus, bases);
        if (ck.bad) ++fails;
    }
    return fails;
}

int main(int argc, char** argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 12;
    m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    g_mem.assign(1 << 24, 0);
    FlatBus bus; bus.m.assign(1 << 24, 0);
    const size_t ncases = sizeof(kCases) / sizeof(kCases[0]);
    int fails = 0, checked = 0, skipped = 0, by_kind[3] = {0, 0, 0};
    for (int v : {5, 6, 15, 24, 25, 26, 27, 28, 29, 30, 31, 32, 40, 47})   // the vectors the translator and the runtime raise
        if (lift::exc_cycles(v) != m68ki_exception_cycle_table[0][v]) { std::printf("exc_cycles(%d) = %d, Musashi %d\n", v, lift::exc_cycles(v), m68ki_exception_cycle_table[0][v]); ++fails; }
    for (size_t ci = 0; ci < ncases; ++ci) {
        const Case& k = kCases[ci];
        const uint16_t op = uint16_t(k.bytes[0] << 8 | k.bytes[1]);
        const int n = k.kind ? trials * 8 : trials;   // more states for SR writes and exceptions (mode, pending level)
        for (int t = 0; t < n; ++t) {
            uint32_t seed = rnd();
            lift::Cpu c; c.bus = &bus;
            for (int i = 0; i < 8; ++i) {
                uint32_t v = rnd() ^ (rnd() << 16);
                if (t & 1) v &= (i % 3 == 0) ? 0xFF : 0xFFFFFF;           // sometimes small values
                c.d[i] = v;
                uint32_t av = 0x100000 + (rnd() & 0xFFFFF) * 2;
                if (t & 2) av = (rnd() & 0xFFFFFF) & ~1u;
                c.a[i] = av;
            }
            c.a[7] = 0x800000 + (rnd() & 0xFFF) * 2;
            c.set_ccr(rnd() & 0x1F);
            int ipl = 0;
            if (k.kind) {   // either mode, interrupts masked on entry, a pending level 0..6 (an SR write may unmask it)
                c.other_sp = 0x700000 + (rnd() & 0xFFF) * 2;
                c.sr_sys = uint16_t(((rnd() & 1) ? 0x2000 : 0) | 0x0700);
                ipl = int(rnd() % 7);
                c.ipl = &ipl;
            }
            std::vector<uint32_t> bases(c.a, c.a + 8);
            bases.insert(bases.end(), c.d, c.d + 8);
            bases.push_back(k.pc);
            bases.push_back(c.a[7]);
            if (k.kind) { bases.push_back(c.other_sp); bases.push_back(0); }   // the other stack; the vector table
            fill(bus, bases, seed | 1);
            for (int i = 0; i < k.size; ++i) { bus.m[(k.pc + i) & 0xFFFFFF] = k.bytes[i]; g_mem[(k.pc + i) & 0xFFFFFF] = k.bytes[i]; }

            m68k_pulse_reset(); m68k_execute(1);
            musashi_set(c, k.pc);
            m68k_set_irq(unsigned(ipl));
            uint32_t npc = 0;
            lift::Exit ex = k.fn(c, npc);
            bool irq = false;
            if (ex.kind == lift::Exit::Irq) { irq = true; npc = lift::interrupt_entry(c, ipl, ex.pc); }   // what System::take_irq does
            else if (ex.kind == lift::Exit::Stop) npc = ex.pc;
            else if (ex.kind != lift::Exit::None) { ++skipped; continue; }          // e.g. privileged in user mode: left to Musashi
            const int used = m68k_execute(1);
            ++checked; ++by_kind[k.kind];
            char label[96];
            std::snprintf(label, sizeof label, "case %zu pc=%X bytes=%02x%02x%02x%02x", ci, k.pc, k.bytes[0], k.bytes[1], k.bytes[2], k.bytes[3]);
            Checker ck{label, fails};
            ck.regs(c, npc, k.kind != 0);
            if (k.kind && op != 0x4E72)   // stop: Musashi's idle accounting, not an instruction cost
                ck.cmp("cycles", uint32_t((c.exc_cycles ? c.exc_cycles : m68ki_cycles[0][op]) + (irq ? lift::exc_cycles(24 + ipl) : 0)), uint32_t(used));
            ck.memory(bus, bases);
            if (ck.bad) { ++fails; break; }
        }
    }
    int irq_checked = 0;
    const int irq_fails = irq_trials(bus, 2000, irq_checked);
    std::printf("checked %d samples (plain %d, sr writes %d, exceptions %d), interrupt entries %d, skipped %d, failing cases %d of %zu, failing interrupt entries %d\n",
                checked, by_kind[0], by_kind[1], by_kind[2], irq_checked, skipped, fails, ncases, irq_fails);
    return fails || irq_fails ? 1 : 0;
}
```

- [ ] **Step 3: Run it to verify it fails**

Run:
```bash
.venv/bin/python tools/lift/gen_tests.py engine/tests/lift_cases.inc 20000
cmake --build engine/build --target lift_diff
```
Expected: compile errors in `lift_diff.cpp`: `no member named 'other_sp' in 'lift::Cpu'` and `no member named 'interrupt_entry' in namespace 'lift'`.

- [ ] **Step 4: Implement the model in `cpu68k.h`**

Replace `struct Exit` (lines 19-24) with:

```cpp
struct Exit {
    // DivZero and Trap are no longer produced (ADR-003 M2 translates them); kept so the numeric kinds stay stable.
    // Irq: an SR write unmasked a pending interrupt; pc is the PC to push. Stop: `stop` executed; pc is the next instruction.
    enum Kind { None, Unsupported, DivZero, Trap, Budget, NotTranslated, Irq, Stop };
    uint32_t pc = 0;
    Kind kind = None;
    int arg = 0;
};
```

Replace `struct Cpu` (lines 40-48) with:

```cpp
struct Cpu {
    uint32_t d[8] = {}, a[8] = {};   // a[7] is the active stack pointer
    uint32_t other_sp = 0;           // the inactive one: USP in supervisor mode, SSP in user mode
    bool x = false, n = false, z = false, v = false, c = false;
    uint16_t sr_sys = 0x2000;        // trace, supervisor and interrupt mask bits (SR & 0xA700)
    uint16_t exc_cycles = 0;         // set by exception_entry(): total cycles of the instruction that raised it
    const int* ipl = nullptr;        // pending interrupt level as the CPU sees it (System::cpu_irq_); nullptr = none
    Bus* bus = nullptr;

    uint32_t ccr() const { return (x << 4) | (n << 3) | (z << 2) | (v << 1) | uint32_t(c); }
    void set_ccr(uint32_t f) { x = f & 16; n = f & 8; z = f & 4; v = f & 2; c = f & 1; }
};
```

After `cond()` (after line 107), add:

```cpp
// ---- Exceptions (ADR-003 M2). What Musashi does on a 68000; lift_diff checks each against it.

// Total cycles of an exception an instruction raises, or of an interrupt entry (Musashi's m68ki_exception_cycle_table[0]).
constexpr int exc_cycles(int vector) {
    return vector == 5 ? 38 : vector == 6 ? 40 : (vector == 15 || (vector >= 24 && vector < 32)) ? 44 : 34;
}

// SR write (move to sr, andi/ori/eori #,sr, rte, stop) like m68ki_set_sr: the stack pointers swap when S changes. Returns true when
// a pending interrupt is now above the mask: Musashi takes it inside this instruction, so generated code returns Exit::Irq.
inline bool set_sr(Cpu& c, uint32_t v) {
    const bool was_s = (c.sr_sys & 0x2000) != 0;
    c.set_ccr(v);
    c.sr_sys = uint16_t(v & 0xA700);
    if (((c.sr_sys & 0x2000) != 0) != was_s) std::swap(c.a[7], c.other_sp);
    return c.ipl && *c.ipl > int((c.sr_sys >> 8) & 7);
}

// m68ki_init_exception: supervisor mode (switching to the SSP), trace off. Returns the SR to stack.
inline uint16_t enter_exception(Cpu& c) {
    const uint16_t sr = uint16_t(c.sr_sys | c.ccr());
    if (!(c.sr_sys & 0x2000)) std::swap(c.a[7], c.other_sp);
    c.sr_sys = uint16_t((c.sr_sys | 0x2000) & ~0x8000);
    return sr;
}

// The 68000's 3-word frame: PC (long), then SR (word) on top.
inline void push_frame(Cpu& c, uint32_t pc, uint16_t sr) {
    c.a[7] -= 4; c.bus->w32(c.a[7], pc);
    c.a[7] -= 2; c.bus->w16(c.a[7], sr);
}

// trap #n (vector 32 + n), chk (6), divide by zero (5), as m68ki_exception_trap / trapN: frame, then the vector.
// pc is the next instruction. Returns the handler address; the instruction's total cost becomes exc_cycles(vector).
inline uint32_t exception_entry(Cpu& c, int vector, uint32_t pc) {
    push_frame(c, pc, enter_exception(c));
    c.exc_cycles = uint16_t(exc_cycles(vector));
    return c.bus->r32(uint32_t(vector) * 4);
}

// Autovectored interrupt entry, as m68ki_exception_interrupt: the mask rises to the level, the vector is read (an empty one
// falls back to vector 15), then the frame. The caller has acknowledged the interrupt and charges exc_cycles(24 + level).
inline uint32_t interrupt_entry(Cpu& c, int level, uint32_t pc) {
    const uint16_t sr = enter_exception(c);
    c.sr_sys = uint16_t((c.sr_sys & ~0x0700) | (level << 8));
    uint32_t npc = c.bus->r32(uint32_t(24 + level) * 4);
    if (npc == 0) npc = c.bus->r32(15 * 4);
    push_frame(c, pc, sr);
    return npc;
}
```

- [ ] **Step 5: Run it to verify it passes**

Run:
```bash
cmake --build engine/build --target lift_diff && engine/build/lift_diff 8
```
Expected: `checked ... (plain ..., sr writes 0, exceptions 0), interrupt entries 2000, skipped ..., failing cases 0 of ..., failing interrupt entries 0`, exit 0. No case is SR-write or exception yet (Tasks 4 and 5 add them). The `exc_cycles` check printed nothing.

- [ ] **Step 6: Check nothing else changed**

Run:
```bash
cmake --build engine/build -j
.venv/bin/python tools/lift/test_gen_program.py
```
Expected: the build succeeds (generated code compiles against the new `Cpu`) and the tests print `OK`. No headless gate is needed: no generated code or runtime behaviour changed.

- [ ] **Step 7: Commit**

```bash
git add engine/src/lift/cpu68k.h engine/tests/lift_diff.cpp tools/lift/gen_tests.py
git commit -m "recomp M2: 68000 exception model in cpu68k.h, lift_diff covers interrupt entry (ADR-003)"
```

---

### Task 3: Interrupts taken in host code; the `irq_blocked` guard removed

**Files:**
- Modify: `engine/src/scd/translate_entry.h` (add `LoopStep`/`loop_step` before the closing `}  // namespace scd`)
- Modify: `engine/tests/translate_entry_test.cpp` (new block before `std::puts("translate_entry_test: ok");`)
- Modify: `engine/src/scd/system.h` (forward declaration; private members after `uint32_t exit_pos_ = 0;` at line 124)
- Modify: `engine/src/scd/system.cpp` (`init` line 214; new helpers after `sub_entries()` at lines 452-467; `run_sub_translated` at lines 469-538; `run_main_translated` at lines 550-585)
- Modify: `tools/lift/test_coverage.py` (`TestTranslatedCensus`, lines 55-62)

**Interfaces:**
- Consumes: Task 2's `lift::Cpu::{other_sp, ipl}`, `lift::interrupt_entry`, `lift::exc_cycles`, `lift::Exit::{Irq, Stop}`.
- Produces:
  - `enum class scd::LoopStep { kDeliver, kIdle, kRun }` and `scd::LoopStep scd::loop_step(bool force_one, bool stopped, int ipl, uint32_t sr)` in `translate_entry.h`.
  - Private `System` members:
    - `void lift_load(lift::Cpu& c)`, `void lift_store(const lift::Cpu& c, uint32_t pc)`: the register file to and from Musashi, inactive SP included
    - `uint32_t take_irq(lift::Cpu& c, int level, uint32_t pc, int32_t& budget)`: acknowledge, clear `stopped_`, charge 44, `lift::interrupt_entry`; returns the handler address
    - `bool stopped_[2]`
  - The runtime contract Tasks 4 and 5 rely on: generated code may return `Exit{pc, Irq}` after an SR write that left `*c.ipl` above the mask (the runtime takes the interrupt at once, pushing `pc`), and `Exit{next, Stop}` after `stop` (the CPU idles until an interrupt is deliverable). `c.ipl` points at `cpu_irq_[cpu]`.

- [ ] **Step 1: Write the failing tests**

In `engine/tests/translate_entry_test.cpp`, before `std::puts("translate_entry_test: ok");`, add:

```cpp
    {   // ADR-003 M2 dispatch loop: deliver above the mask, never twice in a row, idle while stopped
        assert(loop_step(false, false, 6, 0x2500) == LoopStep::kDeliver);   // level 6 above mask 5
        assert(loop_step(false, false, 5, 0x2500) == LoopStep::kRun);       // equal to the mask: blocked
        assert(loop_step(false, false, 0, 0x2000) == LoopStep::kRun);
        assert(loop_step(true, false, 6, 0x2000) == LoopStep::kRun);        // after an entry: the handler's first instruction first
        assert(loop_step(false, true, 2, 0x2300) == LoopStep::kIdle);       // stopped, the pending level is masked
        assert(loop_step(false, true, 4, 0x2300) == LoopStep::kDeliver);    // stopped, an interrupt wakes it
        assert(loop_step(false, false, 4, 0x0300) == LoopStep::kDeliver);   // user mode: the same rule
    }
```

In `tools/lift/test_coverage.py`, replace `test_no_interpreter_rows_and_irq_entries_present` with:

```python
    def test_no_interpreter_or_irq_entry_rows(self):
        # ADR-003 M2: interrupts are taken in host code, so no Musashi step is an interrupt entry.
        f = OUT / "tr.tsv"
        run(TRANS, 400, SCD_COVERAGE=str(f))
        self.assertIn("translated=1", f.read_text().splitlines()[0])
        reasons = {x[2] for x in rows(f)}
        self.assertNotIn("interpreter", reasons)
        self.assertNotIn("irq_entry", reasons)

    def test_interrupts_enter_translated_handlers(self):
        # Main's H-INT handlers are vectored through the Gate Array ($70 reads $A12006) into the IP, its V-INT through the RAM
        # jump slot $FFFD06. After an entry in host code the handler runs as translated code, checked per range like any other:
        # the only Main code stepped in Musashi is the BIOS RAM jump slots at $FFFD00 and up.
        f = OUT / "tr_irq.tsv"
        run(TRANS, 1500, SCD_COVERAGE=str(f))
        main = [x for x in rows(f) if x[0] == "main"]
        self.assertNotIn("irq_entry", {x[2] for x in main})
        self.assertNotIn("code_mismatch", {x[2] for x in main})
        stepped = {int(x[3], 16) for x in main if x[2] == "not_translated"}
        self.assertTrue(all(pc >= 0xFFFD00 for pc in stepped), sorted(hex(pc) for pc in stepped))
```

- [ ] **Step 2: Run them to verify they fail**

Run:
```bash
cmake --build engine/build-interp --target translate_entry_test
SCD_HEADLESS_TR=engine/build/scd_headless .venv/bin/python tools/lift/test_coverage.py -k Translated
```
Expected: the C++ test does not compile (`use of undeclared identifier 'loop_step'`). The Python run fails `test_no_interpreter_or_irq_entry_rows` with `'irq_entry' unexpectedly found` and `test_interrupts_enter_translated_handlers` with an `irq_entry` assertion.

- [ ] **Step 3: Add `loop_step`**

In `engine/src/scd/translate_entry.h`, before `}  // namespace scd`, add:

```cpp
// What the translated-code dispatch loop does next (ADR-003 M2). Interrupts are taken where Musashi's m68k_execute checks them, at the
// top of each step, except right after an entry (force_one): M1's irq_entry Musashi step ran the handler's first instruction with
// the entry, and the loop keeps that. A stopped CPU idles unless an interrupt is deliverable.
enum class LoopStep { kDeliver, kIdle, kRun };

inline LoopStep loop_step(bool force_one, bool stopped, int ipl, uint32_t sr) {
    if (!force_one && ipl > int((sr >> 8) & 7)) return LoopStep::kDeliver;
    if (stopped) return LoopStep::kIdle;
    return LoopStep::kRun;
}
```

Run: `cmake --build engine/build-interp --target translate_entry_test && engine/build-interp/translate_entry_test`
Expected: `translate_entry_test: ok`

- [ ] **Step 4: Declare the runtime members**

In `engine/src/scd/system.h`, directly after `}` closing the `extern "C"` block (before `namespace scd {`), add:

```cpp
namespace lift { struct Cpu; }
```

After `uint32_t exit_pos_ = 0;`, add:

```cpp
    // ADR-003 M2: the translated register file to and from Musashi (the register home between translated runs until M4),
    // and interrupt entry in host code. stopped_: a translated `stop` parked the CPU until an interrupt.
    void lift_load(lift::Cpu& c);
    void lift_store(const lift::Cpu& c, uint32_t pc);
    uint32_t take_irq(lift::Cpu& c, int level, uint32_t pc, int32_t& budget);
    bool stopped_[2] = {false, false};
```

In `engine/src/scd/system.cpp`, `System::init`, after `halted_ = false;` (line 214), add `stopped_[0] = stopped_[1] = false;`.

- [ ] **Step 5: Add the helpers**

In `engine/src/scd/system.cpp`, directly after the closing `}` of `sub_entries()` (line 467), add:

```cpp
// Musashi's SR write neither swaps stacks nor checks interrupts (m68k_set_reg), so the inactive stack pointer travels explicitly:
// USP in supervisor mode, ISP in user mode.
void System::lift_load(lift::Cpu& c) {
    const uint32_t sr = reg_get(M68K_REG_SR);
    for (int i = 0; i < 8; ++i) c.d[i] = reg_get(M68K_REG_D0 + i);
    for (int i = 0; i < 7; ++i) c.a[i] = reg_get(M68K_REG_A0 + i);
    c.a[7] = reg_get(M68K_REG_SP);
    c.other_sp = reg_get((sr & 0x2000) ? M68K_REG_USP : M68K_REG_ISP);
    c.set_ccr(sr & 0x1F);
    c.sr_sys = uint16_t(sr & 0xA700);
}

void System::lift_store(const lift::Cpu& c, uint32_t pc) {
    for (int i = 0; i < 8; ++i) reg_set(M68K_REG_D0 + i, c.d[i]);
    for (int i = 0; i < 7; ++i) reg_set(M68K_REG_A0 + i, c.a[i]);
    reg_set(M68K_REG_SR, c.sr_sys | c.ccr());
    reg_set(M68K_REG_SP, c.a[7]);
    reg_set((c.sr_sys & 0x2000) ? M68K_REG_USP : M68K_REG_ISP, c.other_sp);
    reg_set(M68K_REG_PC, pc);
}

// Interrupt entry in host code, as Musashi's m68ki_exception_interrupt: acknowledge (clears the source, refreshes the IPL),
// then frame and vector (lift::interrupt_entry), 44 cycles. pc is what Musashi would push.
uint32_t System::take_irq(lift::Cpu& c, int level, uint32_t pc, int32_t& budget) {
    int_ack(level);
    stopped_[cur_] = false;
    budget -= lift::exc_cycles(24 + level);
    return lift::interrupt_entry(c, level, pc);
}
```

- [ ] **Step 6: Replace `run_sub_translated`**

Replace the comment and function at lines 469-538 with:

```cpp
// Runs the Sub CPU for `cycles`, executing translated code where available and falling back to Musashi per instruction.
// Interrupts are taken in host code (ADR-003 M2). Returns cycles actually used. cur_ must be kSub.
int System::run_sub_translated(int cycles) {
    static LogBus bus(*this);
    static const bool verify = std::getenv("SCD_LIFT_VERIFY") != nullptr;
    lift::Cpu c;
    c.bus = &bus;
    c.ipl = &cpu_irq_[kSub];          // the level Musashi sees; a translated SR write that unmasks it exits with Exit::Irq
    int32_t budget = cycles;
    bool force_one = false;           // after an entry: run exactly the handler's first instruction before the next check
    while (budget > 0 || force_one) {
        const LoopStep step = loop_step(force_one, stopped_[kSub], cpu_irq_[kSub], reg_get(M68K_REG_SR));
        if (step == LoopStep::kDeliver) {
            lift_load(c);
            lift_store(c, take_irq(c, cpu_irq_[kSub], reg_get(M68K_REG_PC), budget));
            force_one = true;
            continue;
        }
        if (step == LoopStep::kIdle) { budget = 0; break; }   // stopped: Musashi consumes the slice the same way
        uint32_t pc = reg_get(M68K_REG_PC) & 0xFFFFFF;
        if (ovl_track_ && pc >= 0x28000 && pc < 0x80000) note_overlay_exec();   // before translated overlay code can modify its own image
        // First program whose code at pc is in memory exactly as translated (overlays are re-checked after every disc load into PRG RAM).
        bool claimed = false;
        TEntry* te = pick_entry(sub_entries(), prg_ram_, pc, prg_ovl_gen_, claimed);
        fallback_why_ = claimed ? Fallback::kCodeMismatch : Fallback::kNotTranslated;
        const bool single = force_one;
        force_one = false;
        if (te) {
            lift_load(c);
            int32_t one = 1;
            int32_t& b = single ? one : budget;
            int32_t before = b;
            lift::Cpu saved = c;
            uint32_t spc = pc;
            if (verify) { bus.logging = true; bus.writes.clear(); b = 1; before = 1; }
            lift::Exit ex = te->run(c, pc, b);
            if (verify && ex.kind != lift::Exit::NotTranslated && ex.kind != lift::Exit::Unsupported && ex.kind != lift::Exit::Stop) {
                // Undo the writes, replay the same instruction in Musashi with interrupts held off, compare. An SR write that
                // unmasks one exited with Exit::Irq; the interrupt is taken in host code at the top of the loop.
                auto writes = bus.writes; bus.logging = false;
                std::vector<uint8_t> after(writes.size());
                for (size_t i = 0; i < writes.size(); ++i) after[i] = uint8_t(sub_read8(writes[i].first));
                for (size_t i = writes.size(); i-- > 0;) sub_write8(writes[i].first, writes[i].second);
                lift_store(saved, spc);
                m68k_set_irq(0);
                m68k_execute(1);
                m68k_set_irq(unsigned(cpu_irq_[kSub]));
                lift::Cpu m;
                lift_load(m);
                bool bad = false;
                for (auto& w : writes) if (w.first >= 0xFE0000) bad = true;   // I/O side effects (gate array, PCM, BRAM) make a replay meaningless
                for (int i = 0; i < 8 && !bad; ++i) {
                    if (m.d[i] != c.d[i]) { log("[lift-verify] pc=%06x op=%04x d%d lift=%08x musashi=%08x\n", spc, read16(spc), i, c.d[i], m.d[i]); bad = true; }
                    else if (m.a[i] != c.a[i]) { log("[lift-verify] pc=%06x op=%04x a%d lift=%08x musashi=%08x\n", spc, read16(spc), i, c.a[i], m.a[i]); bad = true; }
                }
                if (!bad && m.other_sp != c.other_sp) { log("[lift-verify] pc=%06x op=%04x inactive sp lift=%08x musashi=%08x\n", spc, read16(spc), c.other_sp, m.other_sp); bad = true; }
                if (!bad && (reg_get(M68K_REG_PC) & 0xFFFFFF) != (ex.pc & 0xFFFFFF)) { log("[lift-verify] pc=%06x op=%04x next lift=%06x musashi=%06x\n", spc, read16(spc), ex.pc, reg_get(M68K_REG_PC) & 0xFFFFFF); bad = true; }
                if (!bad && (m.sr_sys | m.ccr()) != (c.sr_sys | c.ccr())) { log("[lift-verify] pc=%06x op=%04x sr lift=%04x musashi=%04x\n", spc, read16(spc), c.sr_sys | c.ccr(), m.sr_sys | m.ccr()); bad = true; }
                for (size_t i = 0; i < writes.size() && !bad; ++i) if (uint8_t(sub_read8(writes[i].first)) != after[i]) { log("[lift-verify] pc=%06x op=%04x mem[%06x] lift=%02x musashi=%02x\n", spc, read16(spc), writes[i].first, after[i], sub_read8(writes[i].first)); bad = true; }
                budget -= 4;
                ++verified_;
                continue;
            }
            bus.logging = false;
            translated_instr_budget_ += uint64_t(before - b);
            if (single) budget -= 1 - one;
            uint32_t npc = ex.pc;
            if (ex.kind == lift::Exit::Irq) npc = take_irq(c, cpu_irq_[kSub], ex.pc, budget);   // taken inside the SR write, as Musashi does
            if (ex.kind == lift::Exit::Stop) stopped_[kSub] = true;
            lift_store(c, npc);
            exit_ring_[exit_pos_++ & 15] = uint32_t(ex.kind) << 28 | (ex.pc & 0xFFFFFF);
            // Budget ends the loop (or, after a single instruction, goes on); NotTranslated re-dispatches: ex.pc may belong to another program.
            if (ex.kind == lift::Exit::Budget || ex.kind == lift::Exit::NotTranslated || ex.kind == lift::Exit::Irq || ex.kind == lift::Exit::Stop) continue;
            fallback_why_ = Fallback::kUnsupported;   // Musashi executes the instruction at ex.pc
        }
        // Interpreter step.
        ++fallback_steps_;
        int used = m68k_execute(1);
        budget -= used > 0 ? used : 4;
        if (sub_wait_vsync_ || halted_) break;   // a HLE call parked the CPU
    }
    return cycles - budget;
}
```

- [ ] **Step 7: Replace `run_main_translated`**

Replace the function at lines 550-585 with:

```cpp
int System::run_main_translated(int cycles) {
    static MainBusAdapter bus(*this);
    lift::Cpu c;
    c.bus = &bus;
    c.ipl = &cpu_irq_[kMain];
    int32_t budget = cycles;
    main_budget_ = &budget;           // VDP DMA stalls are charged here, also during a single handler instruction
    bool force_one = false;
    while (budget > 0 || force_one) {
        const LoopStep step = loop_step(force_one, stopped_[kMain], cpu_irq_[kMain], reg_get(M68K_REG_SR));
        if (step == LoopStep::kDeliver) {
            lift_load(c);
            lift_store(c, take_irq(c, cpu_irq_[kMain], reg_get(M68K_REG_PC), budget));   // vectors through main_read16 ($70: Gate Array H-INT vector)
            force_one = true;
            continue;
        }
        if (step == LoopStep::kIdle) { budget = 0; break; }
        uint32_t pc = reg_get(M68K_REG_PC) & 0xFFFFFF;
        // The IP is checked once per range; the $FFE100 code again after every Main write to its window (main_code_gen_).
        bool claimed = false;
        TEntry* te = pick_entry(main_entries(), main_ram_, pc, main_code_gen_, claimed);
        fallback_why_ = claimed ? Fallback::kCodeMismatch : Fallback::kNotTranslated;
        const bool single = force_one;
        force_one = false;
        if (te) {
            lift_load(c);
            int32_t one = 1;
            lift::Exit ex = te->run(c, pc, single ? one : budget);
            if (single) budget -= 1 - one;
            uint32_t npc = ex.pc;
            if (ex.kind == lift::Exit::Irq) npc = take_irq(c, cpu_irq_[kMain], ex.pc, budget);
            if (ex.kind == lift::Exit::Stop) stopped_[kMain] = true;
            lift_store(c, npc);
            if (halted_) break;
            // NotTranslated re-dispatches: the IP calls into $FFE100 and back.
            if (ex.kind == lift::Exit::Budget || ex.kind == lift::Exit::NotTranslated || ex.kind == lift::Exit::Irq || ex.kind == lift::Exit::Stop) continue;
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
```

- [ ] **Step 8: Build and run the tests**

Run:
```bash
cmake --build engine/build -j && cmake --build engine/build-interp -j --target scd_headless translate_entry_test
engine/build-interp/translate_entry_test
SCD_HEADLESS_TR=engine/build/scd_headless .venv/bin/python tools/lift/test_coverage.py
engine/build/lift_diff 8
```
Expected: `translate_entry_test: ok`. The Python tests print `OK` (the interpreter tests run too). `lift_diff` reports `failing cases 0` and `failing interrupt entries 0`.

- [ ] **Step 9: Gate: bit-identical to the parent**

Run:
```bash
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless work/parent-src/engine/build/scd_headless --frames 60000 --strict-ram --work work/m2/t3
```
Expected: `ok: builds match at every checkpoint`.

If it reports a difference:
- If Task 1 recorded F3 > 0 (branch D3), the loop now uses `cpu_irq_` where M1 decided with `*_irq_level()`. Note the frame and stop: report the first difference and F3 to the user.
- Otherwise it is a bug in this task. Use superpowers:systematic-debugging, starting from the first differing checkpoint.

- [ ] **Step 10: Gate: the census and verify mode**

Run:
```bash
tools/lift/census.sh engine/build/scd_headless work/census/m2-t3 tools/playtest/replays/intro_skip.replay
.venv/bin/python tools/lift/coverage_report.py work/census/m2-t3/intro_skip.tsv --code-dir extracted/code
SCD_LIFT_VERIFY=1 engine/build/scd_headless "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" --frames 3000 --out work/m2/verify 2>&1 | grep -c '\[lift-verify\]'
```
Expected:
- The report has no `irq_entry`, `code_mismatch` or `unsupported` rows. `hle_stub` is 123249 steps. `not_translated` is 896598 steps over 10 PCs: the M1 census's 478615 plus its former `irq_entry` steps at the jump slots `$5F7C` (119391), `$5F82` (238595) and `$FFFD06` (59997). The PCs are Sub `$180 $184 $18A $18E $200 $206 $5F7C $5F82` and Main `$FFFD06 $FFFDAE`.
- These numbers follow exactly when Step 9 passed. Any other `not_translated` PC is a bug: stop and report it.
- The verify count is `0`. M1's `rte` artifact is gone because the replay holds interrupts off. If lines remain, list the first five and stop.

- [ ] **Step 11: Commit**

```bash
git add engine/src/scd/translate_entry.h engine/tests/translate_entry_test.cpp engine/src/scd/system.h engine/src/scd/system.cpp tools/lift/test_coverage.py
git commit -m "recomp M2: interrupts taken in host code, irq_blocked guard removed, stop and Irq exits in the runtime (ADR-003)"
```

---

### Task 4: Translate `trap`, `chk`, divide by zero and `stop`

**Files:**
- Modify: `tools/lift/m68k_decode.py` (`Insn` at lines 18-24; `decode()` except block at lines 144-148; div at lines 216-223; `_group4` `stop` at line 412, `trap` at lines 414-415, `chk` at lines 479-480)
- Modify: `tools/lift/emit.py` (`flow_code`)
- Modify: `tools/lift/gen_program.py` (descent at lines 143 and 183; emission after line 221)
- Modify: `tools/lift/gen_tests.py` (`SYNTHETIC`)
- Modify: `engine/tests/lift_diff.cpp` (sample-class check)
- Create: `tools/lift/test_m68k_decode.py`
- Modify: `tools/lift/test_gen_program.py` (`TestRangeExit`)
- Modify: `mise.toml` (`test-recomp`)

**Interfaces:**
- Consumes: Task 2's `lift::exception_entry`, `lift::set_sr`, `lift::Cpu::exc_cycles`, `lift::Exit::{Irq, Stop}`. Task 3's runtime handling of `Irq` and `Stop`.
- Produces:
  - Flow `('exc',)`: the instruction's code sets `npc` itself (the next instruction, or a handler address after an exception). Descent continues at `pc + size`. `gen_program.py` charges `exc_cycles - m68ki_cycles[0][op]` after it when `c.exc_cycles` is set.
  - `Insn.sr_write: bool`: the instruction writes SR and may return `Exit::Irq`. Task 5 sets it for the other SR writes.
  - `m68k_decode.PRIV`: the C++ format string for the privilege check: `if (!(c.sr_sys & 0x2000)) return lift::Exit{<pc>, lift::Exit::Unsupported};`

- [ ] **Step 1: Write the failing decoder tests**

Create `tools/lift/test_m68k_decode.py`:

```python
"""Decoder output for exceptions and SR writes (ADR-003 M2). Synthetic encodings, no disc needed."""
import pathlib, sys, unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from m68k_decode import decode
from emit import flow_code

PC = 0x1000


def dec(hexs):
    raw = bytes.fromhex(hexs) + bytes(16)
    r16 = lambda a: (raw[a - PC] << 8 | raw[a - PC + 1]) if 0 <= a - PC < len(raw) - 1 else 0
    ins = decode(r16, PC)
    return ins, '\n'.join(ins.code + flow_code(ins))


class TestExceptions(unittest.TestCase):
    def test_trap_uses_vector_32_plus_n_and_continues(self):
        for n in (0, 7, 15):
            ins, code = dec('%04X' % (0x4E40 + n))
            self.assertEqual(ins.flow, ('exc',))
            self.assertIn('npc = lift::exception_entry(c, %d, 0x1002);' % (32 + n), code)
            self.assertNotIn('Exit::Trap', code)

    def test_chk(self):
        ins, code = dec('4181')                                   # chk.w d1,d0
        self.assertEqual(ins.flow, ('exc',))
        self.assertIn('int16_t(c.d[0])', code)
        self.assertIn('npc = lift::exception_entry(c, 6, 0x1002);', code)
        ins, code = dec('45BC0010')                               # chk.w #$10,d2
        self.assertEqual(ins.size, 4)
        self.assertIn('int16_t(c.d[2])', code)
        self.assertIn('npc = lift::exception_entry(c, 6, 0x1004);', code)

    def test_divide_by_zero_raises_vector_5(self):
        for hexs in ('80FC0000', '83FC0000'):                    # divu #0,d0 ; divs #0,d1
            ins, code = dec(hexs)
            self.assertEqual(ins.flow, ('exc',))
            self.assertIn('npc = lift::exception_entry(c, 5, 0x1004);', code)
            self.assertNotIn('DivZero', code)

    def test_stop(self):
        ins, code = dec('4E722000')
        self.assertEqual((ins.flow, ins.size, ins.sr_write), (('exc',), 4, True))
        self.assertIn('lift::Exit::Unsupported', code.splitlines()[0])   # user mode: Musashi takes the privilege violation
        self.assertIn('if (lift::set_sr(c, 0x2000)) return lift::Exit{0x1004, lift::Exit::Irq};', code)
        self.assertIn('return lift::Exit{0x1004, lift::Exit::Stop};', code)

    def test_plain_instruction_is_not_an_sr_write(self):
        ins, _ = dec('7001')                                      # moveq #1,d0
        self.assertFalse(ins.sr_write)


if __name__ == '__main__':
    unittest.main()
```

In `tools/lift/test_gen_program.py`, in `TestRangeExit`, after `test_sub_runs_across_ranges`, add:

```python
    def test_trap_is_translated_and_descent_continues(self):
        # $28000: trap #0 ; moveq #1,d0 ; rts. The handler returns to $28002, so it is decoded too.
        d = self.code / "overlays"
        d.mkdir()
        (d / "ovl_0000c0de.bin").write_bytes(bytes.fromhex("4E4070014E75") + bytes(0x100))
        self.seeds("ovl_0000c0de", 0x28000)
        self.gen("ovl:overlays/ovl_0000c0de.bin")
        cpp = (self.out / "ovl_0000c0de_translated.cpp").read_text()
        self.assertIn("case 0x28002:", cpp)
        self.assertIn("budget -= int32_t(c.exc_cycles) - m68ki_cycles[0][0x4E40];", cpp)
        kind, pc, d0 = self.run_prog("ovl_0000c0de_translated.cpp", "ovl_0000c0de", 0x28000)
        self.assertEqual((kind, pc, d0), (5, 0x0, 0))   # vector 32 is 0 in the harness: NotTranslated at the handler
```

- [ ] **Step 2: Run them to verify they fail**

Run: `.venv/bin/python tools/lift/test_m68k_decode.py; .venv/bin/python tools/lift/test_gen_program.py -k trap`
Expected: FAIL. The trap test reports `('stop',) != ('exc',)`, the chk and stop tests show `unsupported` code, and `test_plain_instruction_is_not_an_sr_write` raises `AttributeError: 'Insn' object has no attribute 'sr_write'`. The `gen_program` test fails on `case 0x28002:`.

- [ ] **Step 3: Implement the decoder changes**

In `tools/lift/m68k_decode.py`:

Update the docstring's flow list (line 10) to:

```python
  ('ret',) ('rte',) ('stop',)
  ('exc',)          the code sets npc (next instruction, or a handler after an exception); descent continues at pc+size
```

In `class Insn`, after `flow: object = 'next'`, add:

```python
    sr_write: bool = False      # writes SR: may return Exit::Irq (ADR-003 M2)
```

After `def hx(v): ...` add:

```python
# A privileged instruction in user mode leaves translated code: Musashi takes the privilege violation (the game runs in supervisor
# mode, JOURNAL "Recomp M2 investigation"). It must come before any side effect of the instruction.
PRIV = 'if (!(c.sr_sys & 0x2000)) return lift::Exit{%s, lift::Exit::Unsupported};'
```

In `decode()`, in the `except Unsupported` block, after `ins.size = 2`, add `ins.sr_write = False`.

Replace the div block (lines 216-223, from `if top == 8:` through the line ending `% r9))`) with:

```python
            if top == 8:
                signed = bool(op & 0x100)
                nxt = hx(ctx.pos)
                body = (('int32_t dd = int32_t(c.d[%d]); int32_t dv = int16_t(%s); int64_t q = dd / dv; int32_t r = dd %% dv; ' % (r9, s) +
                         'if (q > 32767 || q < -32768) { c.v = true; } else { c.d[%d] = (uint32_t(uint16_t(r)) << 16) | uint16_t(q); c.n = q < 0; c.z = q == 0; c.v = false; c.c = false; }' % r9)
                        if signed else
                        ('uint32_t dd = c.d[%d]; uint32_t dv = %s & 0xFFFF; uint32_t q = dd / dv; uint32_t r = dd %% dv; ' % (r9, s) +
                         'if (q > 0xFFFF) { c.v = true; } else { c.d[%d] = (r << 16) | q; c.n = q & 0x8000; c.z = q == 0; c.v = false; c.c = false; }' % r9))
                # divide by zero: vector 5, the frame holds the next instruction, flags unchanged (Musashi)
                code.append('if (%s == 0) { npc = lift::exception_entry(c, 5, %s); } else { %s npc = %s; }' % (s, nxt, body, nxt))
                ins.flow = ('exc',)
```

The `else:` branch (mulu/muls) and the `return` after it stay as they are.

In `_group4`, replace `if op == 0x4E72: raise Unsupported('stop')` with:

```python
    if op == 0x4E72:  # stop #imm: set SR, then idle until an interrupt (an already deliverable one exits with Exit::Irq)
        imm = ctx.ext()
        code.append(PRIV % hx(pc))
        code.append('if (lift::set_sr(c, %s)) return lift::Exit{%s, lift::Exit::Irq};' % (hx(imm), hx(pc + 4)))
        code.append('return lift::Exit{%s, lift::Exit::Stop};' % hx(pc + 4))
        ins.flow = ('exc',); ins.sr_write = True; return
```

Replace the trap lines

```python
    if op & 0xFFF0 == 0x4E40:
        code.append('return lift::Exit{%s, lift::Exit::Trap, %d};' % (hx(pc), op & 15)); ins.flow = ('stop',); return
```

with

```python
    if op & 0xFFF0 == 0x4E40:  # trap #n: vector 32 + n, the frame holds the next instruction
        code.append('npc = lift::exception_entry(c, %d, %s);' % (32 + (op & 15), hx(pc + 2))); ins.flow = ('exc',); return
```

Replace

```python
    if (op & 0xF1C0) == 0x4180:  # chk
        raise Unsupported('chk')
```

with

```python
    if (op & 0xF1C0) == 0x4180:  # chk.w <ea>,Dn: Z from Dn, V and C cleared, N only when it traps (Musashi)
        check(mode, reg, 'data')
        ea = Operand(ctx, code, mode, reg, 2); b = ctx.t()
        code.append('uint32_t %s = %s;' % (b, ea.read()))
        code.append('{ int32_t v = int16_t(c.d[%d]), lim = int16_t(%s); c.z = v == 0; c.v = false; c.c = false; '
                    'if (v >= 0 && v <= lim) npc = %s; else { c.n = v < 0; npc = lift::exception_entry(c, 6, %s); } }' % (r9, b, hx(ctx.pos), hx(ctx.pos)))
        ins.flow = ('exc',); return
```

In `tools/lift/emit.py`, after `if f == ('self',): return []`, add:

```python
    if f == ('exc',): return []           # instruction code already set npc (ADR-003 M2)
```

In `tools/lift/gen_program.py`, change both descent conditions (lines 143 and 183) from `f[0] in ('cond', 'call', 'call_dyn')` to `f[0] in ('cond', 'call', 'call_dyn', 'exc')`. Use one edit with `replace_all`: the string `f[0] in ('cond', 'call', 'call_dyn'))` occurs exactly twice.

After line 221 (`for line in ins.code + flow_code(ins): out.append('            ' + line)`), add:

```python
        if ins.flow == ('exc',):   # an exception the instruction raised costs exc_cycles(vector) in all (ADR-003 M2)
            out.append('            if (c.exc_cycles) { budget -= int32_t(c.exc_cycles) - m68ki_cycles[0][0x%04X]; c.exc_cycles = 0; }' % op)
```

- [ ] **Step 4: Run the decoder tests**

Run: `.venv/bin/python tools/lift/test_m68k_decode.py && .venv/bin/python tools/lift/test_gen_program.py`
Expected: both print `OK`.

- [ ] **Step 5: Add the synthetic cases and the sample-class check**

In `tools/lift/gen_tests.py`, replace `SYNTHETIC = []` with:

```python
SYNTHETIC = [
    *['%04X' % (0x4E40 + n) for n in range(16)],            # trap #0..#15
    '4181', '4190', '45BC0010',                            # chk.w d1,d0 / (a0),d0 / #$10,d2
    '80FC0000', '83FC0000', '80C1',                        # divu #0,d0 / divs #0,d1 / divu d1,d0
    '4E722000', '4E722700', '4E720000',                    # stop #$2000 / #$2700 / #$0000 (to user mode)
]
```

In `engine/tests/lift_diff.cpp`, replace `return fails || irq_fails ? 1 : 0;` with:

```cpp
    if (by_kind[1] == 0 || by_kind[2] == 0) { std::printf("FAIL: no SR-write or exception samples were checked\n"); return 1; }
    return fails || irq_fails ? 1 : 0;
```

Run:
```bash
.venv/bin/python tools/lift/gen_tests.py engine/tests/lift_cases.inc 20000
cmake --build engine/build --target lift_diff && engine/build/lift_diff 8
```
Expected: `sr writes` > 0 (the three `stop` cases), `exceptions` > 0 (the synthetic cases plus every disc `div`), `failing cases 0`, `failing interrupt entries 0`, exit 0.

- [ ] **Step 6: Gate: what the translated programs gained, and bit-identity**

`chk` previously stopped recursive descent and the `ovl909` linear scan. In the overlays it is data (Task 1 F4), so more data bytes get decoded there. That can merge code ranges and change their CRCs.

Run:
```bash
mkdir -p work/m2 && cp -R engine/build/generated work/m2/gen-before
cmake --build engine/build -j
for f in work/m2/gen-before/*_translated.cpp; do n=$(basename "$f"); grep -o 'case 0x[0-9A-F]*:' "$f" | sort > work/m2/a.txt; grep -o 'case 0x[0-9A-F]*:' "engine/build/generated/$n" | sort > work/m2/b.txt; cmp -s work/m2/a.txt work/m2/b.txt || echo "case set changed: $n ($(wc -l < work/m2/a.txt) -> $(wc -l < work/m2/b.txt))"; done
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless work/parent-src/engine/build/scd_headless --frames 60000 --strict-ram --work work/m2/t4
tools/lift/census.sh engine/build/scd_headless work/census/m2-t4 tools/playtest/replays/intro_skip.replay
.venv/bin/python tools/lift/coverage_report.py work/census/m2-t4/intro_skip.tsv --code-dir extracted/code
```
Expected:
- Case sets change only for overlay programs that contained `chk` (`ovl909`, `ovl_936869c7`, `ovl_a7b6f72d`). `sub` and `main` are unchanged.
- `ok: builds match at every checkpoint`.
- The census is identical to Task 3's: no `code_mismatch`, same `not_translated` and `hle_stub` totals.

If the strict run or the census differs, the merged overlay ranges are the suspect. Compare the `k...Ranges` tables of the changed programs before and after, then stop and report.

- [ ] **Step 7: Add the decoder test to the mise task**

In `mise.toml`, in `[tasks.test-recomp]`, after `"python tools/lift/test_gen_program.py",`, add `"python tools/lift/test_m68k_decode.py",`.

Run: `mise run test-recomp`
Expected: every test passes.

- [ ] **Step 8: Commit**

```bash
git add tools/lift/m68k_decode.py tools/lift/emit.py tools/lift/gen_program.py tools/lift/gen_tests.py engine/tests/lift_diff.cpp tools/lift/test_m68k_decode.py tools/lift/test_gen_program.py mise.toml
git commit -m "recomp M2: translate trap, chk, divide by zero and stop; exception cycles (ADR-003)"
```

---

### Task 5: SR writes: `rte`, `move to sr`, `andi/ori/eori #,sr`

**Files:**
- Modify: `tools/lift/m68k_decode.py` (`_group0` to-SR block at lines 369-375; `_group4` `rte` at lines 408-409, `move to sr` at lines 457-458)
- Modify: `tools/lift/emit.py` (import; the `rte` line)
- Modify: `tools/lift/gen_tests.py` (case filter at line 22; `SYNTHETIC`)
- Modify: `tools/lift/test_m68k_decode.py`

**Interfaces:**
- Consumes: `lift::set_sr`, `PRIV`, `Insn.sr_write`, the runtime's `Exit::Irq` handling (Task 3), `lift_diff` setup class 1 (Task 2).
- Produces: every SR write goes through `lift::set_sr`. It checks privilege before any side effect. It returns `Exit{next pc, Irq}` when a pending level is now above the mask (`rte`: the popped PC). `eori #,sr` now changes the system byte (it only changed the CCR before).

- [ ] **Step 1: Write the failing tests**

Append to `tools/lift/test_m68k_decode.py`, before `if __name__ == '__main__':`:

```python
class TestSrWrites(unittest.TestCase):
    def test_rte(self):
        ins, code = dec('4E73')
        self.assertEqual(ins.flow, ('rte',))
        self.assertTrue(ins.sr_write)
        self.assertIn('lift::Exit::Unsupported', code.splitlines()[0])
        self.assertIn('if (lift::set_sr(c, f)) return lift::Exit{npc, lift::Exit::Irq};', code)

    def test_move_to_sr_checks_privilege_before_the_ea(self):
        ins, code = dec('46DF')                                   # move (sp)+,sr
        self.assertTrue(ins.sr_write)
        self.assertLess(code.index('Exit::Unsupported'), code.index('c.a[7] += 2'))
        self.assertIn('return lift::Exit{0x1002, lift::Exit::Irq};', code)
        ins, code = dec('46FC2700')                               # move #$2700,sr
        self.assertIn('return lift::Exit{0x1004, lift::Exit::Irq};', code)

    def test_logic_to_sr(self):
        for hexs, op in (('027CF8FF', '&'), ('007C0700', '|'), ('0A7C0700', '^')):
            ins, code = dec(hexs)
            self.assertTrue(ins.sr_write, hexs)
            self.assertIn('lift::set_sr(c, (c.sr_sys | c.ccr()) %s 0x' % op, code)
            self.assertIn('return lift::Exit{0x1004, lift::Exit::Irq};', code)

    def test_logic_to_ccr_is_not_an_sr_write(self):
        ins, code = dec('023C00FE')                               # andi #$FE,ccr
        self.assertFalse(ins.sr_write)
        self.assertNotIn('set_sr', code)

    def test_move_from_sr_is_not_privileged(self):
        ins, code = dec('40C0')                                   # move sr,d0 (unprivileged on a 68000)
        self.assertFalse(ins.sr_write)
        self.assertNotIn('Unsupported', code)
```

Run: `.venv/bin/python tools/lift/test_m68k_decode.py -k TestSrWrites`
Expected: FAIL (`rte` is not `sr_write`; the to-SR code has no `set_sr`).

- [ ] **Step 2: Implement**

In `tools/lift/m68k_decode.py`, `_group0`, replace the block

```python
    if mode == 7 and reg == 4:  # to ccr / sr
        if B == 4 or kind in (2, 3, 6): raise Unsupported('imm to imm')
        if kind == 0: code.append('c.set_ccr(c.ccr() | %s);' % hx(imm & 0x1F)) if B == 1 else code.append('c.set_ccr(c.ccr() | %s); c.sr_sys |= %s;' % (hx(imm & 0x1F), hx(imm & 0xA700)))
        elif kind == 1: code.append('c.set_ccr(c.ccr() & %s);' % hx(imm & 0x1F)) if B == 1 else code.append('c.set_ccr(c.ccr() & %s); c.sr_sys &= %s;' % (hx(imm & 0x1F), hx(imm & 0xA700)))
        elif kind == 5: code.append('c.set_ccr(c.ccr() ^ %s);' % hx(imm & 0x1F))
        else: raise Unsupported('imm to ccr kind')
        return
```

with

```python
    if mode == 7 and reg == 4:  # ori/andi/eori to ccr (byte) or sr (word)
        if B == 4 or kind in (2, 3, 6): raise Unsupported('imm to imm')
        opx = {0: '|', 1: '&', 5: '^'}[kind]
        if B == 1:
            code.append('c.set_ccr(c.ccr() %s %s);' % (opx, hx(imm & 0x1F)))
            return
        code.append(PRIV % hx(ctx.pc))
        code.append('if (lift::set_sr(c, (c.sr_sys | c.ccr()) %s %s)) return lift::Exit{%s, lift::Exit::Irq};' % (opx, hx(imm), hx(ctx.pos)))
        ins.sr_write = True
        return
```

In `_group4`, replace

```python
    if op == 0x4E73:
        ins.flow = ('rte',); return
```

with

```python
    if op == 0x4E73:  # rte: code in emit.flow_code
        ins.flow = ('rte',); ins.sr_write = True; return
```

Replace

```python
    if (op & 0xFFC0) == 0x46C0:  # move to sr
        ea = Operand(ctx, code, mode, reg, 2); v = ctx.t(); code.append('uint32_t %s = %s; c.set_ccr(%s); c.sr_sys = %s & 0xA700;' % (v, ea.read(), v, v)); return
```

with

```python
    if (op & 0xFFC0) == 0x46C0:  # move to sr: the privilege check comes before the EA's side effects ((sp)+)
        check(mode, reg, 'data')
        code.append(PRIV % hx(pc))
        ea = Operand(ctx, code, mode, reg, 2); v = ctx.t()
        code.append('uint32_t %s = %s; if (lift::set_sr(c, %s)) return lift::Exit{%s, lift::Exit::Irq};' % (v, ea.read(), v, hx(ctx.pos)))
        ins.sr_write = True; return
```

In `tools/lift/emit.py`, change the import to `from m68k_decode import hx, PRIV` and replace the `rte` line with:

```python
    if f[0] == 'rte': return [PRIV % hx(ins.pc),   # pull SR, then PC; then the SR write (stack swap, a pending interrupt), as Musashi
                              '{ uint32_t f = c.bus->r16(c.a[7]); c.a[7] += 2; npc = c.bus->r32(c.a[7]); c.a[7] += 4; if (lift::set_sr(c, f)) return lift::Exit{npc, lift::Exit::Irq}; }']
```

In `tools/lift/gen_tests.py`:

Replace the filter (line 22)

```python
            if key not in seen and ins.flow != ('rte',) and data[pc - base:pc - base + 2][0] != 0x46 and 'unsupported' not in (ins.code[0] if ins.code else ''):
```

with

```python
            if key not in seen and 'unsupported' not in (ins.code[0] if ins.code else ''):
```

At the end of the `SYNTHETIC` list, add:

```python
    '4E73',                                                # rte
    '46FC2000', '46FC2700', '46FC0000',                    # move #imm,sr (#$0000: to user mode, unmasked)
    '46DF', '46C0', '46D0',                                # move (sp)+,sr / d0,sr / (a0),sr
    '027CF8FF', '027CDFFF', '007C0700', '007C2000',        # andi/ori #imm,sr ($DFFF: to user mode)
    '0A7C0700', '0A7C2000',                                # eori #imm,sr
```

- [ ] **Step 3: Run the tests and `lift_diff`**

Run:
```bash
.venv/bin/python tools/lift/test_m68k_decode.py
.venv/bin/python tools/lift/gen_tests.py engine/tests/lift_cases.inc 20000
cmake --build engine/build --target lift_diff && engine/build/lift_diff 8
```
Expected: `OK`. `lift_diff` reports `sr writes` in the thousands (the disc's `rte`, `move to sr` and logic-to-SR instructions plus the synthetic ones; `skipped` grows by the user-mode halves), `failing cases 0`, `failing interrupt entries 0`, exit 0.

- [ ] **Step 4: Gate with branch D1**

Run:
```bash
cmake --build engine/build -j
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless work/parent-src/engine/build/scd_headless --frames 60000 --strict-ram --work work/m2/t5
SCD_LIFT_VERIFY=1 engine/build/scd_headless "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" --frames 3000 --out work/m2/verify 2>&1 | grep -c '\[lift-verify\]'
```

**If Task 1 recorded F1 = 0:** expected `ok: builds match at every checkpoint` and a verify count of `0`. Go to Step 5.

**If F1 > 0:** Musashi takes those interrupts inside the SR write. The parent took them at the next dispatch step, which is usually the next slice, so this task moves interrupt timing toward the interpreter and away from the parent. Expected: lift_diff and verify are clean, and the strict run differs. Then:

1. Record the first difference (`run.py` prints it).
2. Compare against the interpreter:

   ```bash
   .venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless engine/build-interp/scd_headless --frames 60000 --work work/m2/t5_interp
   ```

   Note the first visible divergence frame against F5 (the parent's).
3. **Stop and ask the user, without committing.** Give them the two frames and the two options:
   - **(a) Accept Musashi semantics** and make this build the new strict-RAM reference. Rebuild `work/parent-src` from this commit after it is committed, and record the decision in the journal.
   - **(b) Keep the parent's timing.** In the three SR-write emitters (`_group0`, `move to sr`, `emit.py` `rte`), drop the `if (...) return lift::Exit{..., lift::Exit::Irq};` wrapper, keeping a plain `lift::set_sr(c, ...);`. Record "SR writes do not take interrupts immediately" as a known infidelity for M3. In `lift_diff`, give kind-1 cases `ipl = 0`, so Musashi takes no interrupt either.

   Continue with Task 6 meanwhile. Task 7 reports M2 blocked until the user decides.

- [ ] **Step 5: Commit**

```bash
git add tools/lift/m68k_decode.py tools/lift/emit.py tools/lift/gen_tests.py tools/lift/test_m68k_decode.py
git commit -m "recomp M2: SR writes (rte, move to sr, andi/ori/eori #,sr) swap stacks and take unmasked interrupts (ADR-003)"
```

---

### Task 6: Host-built BIOS glue reported as `hle_stub`; the M2 gate

This resolves the 8 unmapped glue PCs left open by M1 (ADR-003 Progress) and the jump-slot PCs Task 3 added.

**Files:**
- Modify: `tools/lift/coverage_report.py` (constants at lines 7-11, `merge` at lines 47-52, `report` at lines 73-90, `main` at lines 93-110)
- Modify: `tools/lift/test_coverage_report.py`
- Modify: `tools/lift/test_coverage.py` (`TestTranslatedCensus`)

**Interfaces:**
- Consumes: the census TSV format (M1). The glue addresses come from `System::build_sub_bios` (`engine/src/scd/system.cpp:267-315`: level-2 wrapper `$180..$190`, `rte` at `$190`, boot stub `$200..$212`, level 1..7 slots at `$5F70 + 6n`) and `build_main_rom` (`system.cpp:228-265`: `rte` at `$9A4`, slots `$FFFD06..$FFFD7E`, `$FFFDA8..$FFFDB4`).
- Produces:
  - `HOST_GLUE: dict[str, tuple[tuple[int, int], ...]]` and `host_glue(cpu: str, pc: int) -> bool`.
  - `merge()` reports `not_translated` rows at glue PCs as `hle_stub`.
  - `GATES = {'m1': ..., 'm2': ...}`, `report(total, sizes, gate='m1')`, and the CLI option `--gate {m1,m2}` (default `m1`). The `m2` gate fails on `not_translated`, `code_mismatch`, `irq_entry`, `unsupported` and `interpreter`.

- [ ] **Step 1: Write the failing tests**

Append to `tools/lift/test_coverage_report.py`, before `if __name__ == "__main__":`:

```python
class TestHostGlue(Base):
    def run_main(self, *args):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = cr.main(list(args))
        return rc, buf.getvalue()

    def test_glue_is_reported_as_hle_stub(self):
        a = self.census("a.tsv",
                        "sub\t00000000\tnot_translated\t000180\t5\n"       # level-2 wrapper
                        "sub\t00000000\tnot_translated\t00018e\t5\n"
                        "sub\t00000000\tnot_translated\t000206\t1\n"       # boot stub
                        "sub\t00000000\tnot_translated\t005f82\t5\n"       # level-3 jump slot
                        "main\t00000000\tnot_translated\tfffd06\t5\n"      # V-INT jump slot
                        "main\t00000000\tnot_translated\tfffdae\t1\n")     # Main _BURAM jump slot
        self.assertEqual({k[2] for k in cr.merge([a])}, {"hle_stub"})
        rc, out = self.run_main(a, "--code-dir", self.code, "--check", "--gate", "m2")
        self.assertEqual(rc, 0, out)

    def test_glue_bounds(self):
        self.assertTrue(cr.host_glue("sub", 0x190))
        self.assertFalse(cr.host_glue("sub", 0x192))
        self.assertFalse(cr.host_glue("sub", 0x5F22))      # _CDBIOS: an HLE stub the runtime already labels
        self.assertTrue(cr.host_glue("main", 0xFFFD78))
        self.assertFalse(cr.host_glue("main", 0xFFFD7E))
        self.assertFalse(cr.host_glue("main", 0xFF0EFA))   # IP code is never glue
        self.assertFalse(cr.host_glue("main", 0x000180))   # glue ranges are per CPU

    def test_m2_gate_rejects_irq_entry_and_unsupported(self):
        a = self.census("a.tsv",
                        "main\t00000000\tirq_entry\tff0584\t9\n"
                        "sub\t00000000\tunsupported\t00d600\t9\n"
                        "main\t00000000\thle_stub\t00036c\t9\n")
        rc, _ = self.run_main(a, "--code-dir", self.code, "--check", "--gate", "m1")
        self.assertEqual(rc, 0)
        rc, out = self.run_main(a, "--code-dir", self.code, "--check", "--gate", "m2")
        self.assertEqual(rc, 1)
        self.assertIn("irq_entry", out)
        self.assertIn("unsupported", out)
```

In `tools/lift/test_coverage.py`, add after the imports:

```python
import sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import coverage_report as cr
```

Add to `TestTranslatedCensus`:

```python
    def test_only_hle_steps_remain(self):
        # ADR-003 M2 exit criterion on a short run: with host-built BIOS glue counted as HLE, every Musashi step is hle_stub.
        f = OUT / "tr_m2.tsv"
        run(TRANS, 1500, SCD_COVERAGE=str(f))
        self.assertEqual({k[2] for k in cr.merge([str(f)])}, {"hle_stub"})
```

Run: `.venv/bin/python tools/lift/test_coverage_report.py`
Expected: FAIL. `module 'coverage_report' has no attribute 'host_glue'`, and the gate tests fail because of the unknown `--gate` argument.

- [ ] **Step 2: Implement**

In `tools/lift/coverage_report.py`, replace

```python
GATE_REASONS = ('not_translated', 'code_mismatch')
```

with

```python
# m1: nothing runs in the interpreter for want of translation. m2: only HLE (stubs and host-built glue) runs in it.
GATES = {'m1': ('not_translated', 'code_mismatch'),
         'm2': ('not_translated', 'code_mismatch', 'irq_entry', 'unsupported', 'interpreter')}
# Host-built BIOS glue (System::build_sub_bios / build_main_rom in engine/src/scd/system.cpp): RAM jump slots, the level-2
# wrapper, the boot stub, rte stubs. Not disc code, so its Musashi steps count as HLE (ADR-003 M2). Keep in step with system.cpp.
HOST_GLUE = {
    'sub': ((0x180, 0x192),            # level-2 wrapper (movem, jsr $60BE, movem, rte) and the rte at $190
            (0x200, 0x212),            # boot stub
            (0x5F76, 0x5FA0)),         # level 1..7 RAM jump slots ($5F70 + 6n)
    'main': ((0x0009A4, 0x0009A6),     # rte behind unpatched RAM slots
             (0xFFFD06, 0xFFFD7E),     # interrupt RAM jump slots
             (0xFFFDA8, 0xFFFDB4)),    # jump slots ($FFFDAE: the Main-side _BURAM)
}


def host_glue(cpu, pc):
    return any(lo <= pc < hi for lo, hi in HOST_GLUE.get(cpu, ()))
```

Replace `merge`:

```python
def merge(paths):
    total = collections.Counter()
    for p in paths:
        for cpu, image, reason, pc, count in read_census(p):
            if reason == 'not_translated' and host_glue(cpu, pc):
                reason = 'hle_stub'
            total[(cpu, image, reason, pc)] += count
    return total
```

In `report`, change the signature to `def report(total, sizes, gate='m1'):`, and replace both uses of `GATE_REASONS` in its body with `GATES[gate]`.

In `main`, add after the `--check` argument:

```python
    ap.add_argument('--gate', choices=sorted(GATES), default='m1', help='causes --check rejects (m1: not_translated, code_mismatch; m2: anything but hle_stub)')
```

Replace `text, failures = report(total, sizes)` with `text, failures = report(total, sizes, a.gate)`, and the FAIL line with:

```python
        print('FAIL: %d PCs ran in the interpreter for a cause the %s gate rejects' % (failures, a.gate))
```

- [ ] **Step 3: Run the tests**

Run:
```bash
.venv/bin/python tools/lift/test_coverage_report.py
SCD_HEADLESS_TR=engine/build/scd_headless .venv/bin/python tools/lift/test_coverage.py -k Translated
.venv/bin/python tools/lift/coverage_report.py work/census/m2-t4/intro_skip.tsv --code-dir extracted/code --check --gate m2
```
Expected: `OK`, `OK`, and the report has only an `hle_stub` row (1019847 steps if Task 3's numbers held: 123249 + 896598) with exit 0.

- [ ] **Step 4: Commit**

```bash
git add tools/lift/coverage_report.py tools/lift/test_coverage_report.py tools/lift/test_coverage.py
git commit -m "recomp M2: host-built BIOS glue reported as hle_stub, coverage --gate m2 (ADR-003)"
```

---

### Task 7: The M2 gate, mise, and documentation

**Files:**
- Modify: `mise.toml` (`[tasks.coverage]`)
- Modify: `docs/JOURNAL.md` (append)
- Modify: `docs/ADR-003-static-recompilation.md` (M2 row at line 40, Plans at lines 71-74, Progress)
- Modify: `CLAUDE.md` (line 62)

**Interfaces:**
- Consumes: everything above.
- Produces: `mise run coverage` gates on `--gate m2`. The M2 numbers go into the journal and ADR.

- [ ] **Step 1: Full test pass**

Run:
```bash
mise run test-recomp
.venv/bin/python tools/lift/gen_tests.py engine/tests/lift_cases.inc 20000 && cmake --build engine/build --target lift_diff && engine/build/lift_diff 8
cmake --build engine/build -j
```
Expected: all tests pass. `lift_diff` reports `sr writes` > 0, `exceptions` > 0, `interrupt entries 2000`, `failing cases 0`, `failing interrupt entries 0`.

- [ ] **Step 2: The M2 exit criterion on the reference replays**

Run:
```bash
tools/lift/census.sh engine/build/scd_headless work/census/m2 tools/playtest/replays/*.replay
.venv/bin/python tools/lift/coverage_report.py work/census/m2/*.tsv --code-dir extracted/code --check --gate m2
```
Expected: exit 0, with the report's only cause `hle_stub`. If it fails, the "top gate PCs" list names them. Glue missing from `HOST_GLUE` means checking `system.cpp` and extending the table (with a `test_glue_bounds` line). Anything else is a translator or runtime gap: stop and report it.

- [ ] **Step 3: The bit-identity gates**

Run:
```bash
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless work/parent-src/engine/build/scd_headless --frames 60000 --strict-ram --work work/m2/final
.venv/bin/python tools/playtest/run.py tools/playtest/replays/intro_skip.replay --headless engine/build/scd_headless --compare-headless engine/build-interp/scd_headless --frames 60000 --work work/m2/final_interp
SCD_LIFT_VERIFY=1 engine/build/scd_headless "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" --frames 3000 --out work/m2/verify 2>&1 | grep -c '\[lift-verify\]'
```
Expected:
- The first command prints `ok: builds match at every checkpoint`. In branch D1 (a), compare against the rebuilt reference instead. In branch D1 (b), against `work/parent-src` as written.
- The second prints the first visible divergence vs the interpreter. Record it next to F5. It must not be earlier than F5.
- The verify count is `0`.

- [ ] **Step 4: Switch the coverage task to the M2 gate**

In `mise.toml`, in `[tasks.coverage]`, change the description to `"ADR-003 gate (M2): run the reference replays on the translated build; fails if anything but HLE ran in the interpreter"` and the last command to:

```toml
  "python tools/lift/coverage_report.py work/census/translated/*.tsv --code-dir extracted/code --seeds-out extracted/code/seeds --check --gate m2",
```

- [ ] **Step 5: Document**

Append to `docs/JOURNAL.md` (fill in the numbers Steps 1 to 3 printed, and use the date of the run):

```markdown
## 2026-10-05: Recomp M2 exceptions in host code (ADR-003)
- Interrupts are taken in host code (`System::take_irq`, `lift::interrupt_entry`) where Musashi takes them: at the top of each dispatch step, with the handler's first instruction run before the next check (as M1's `irq_entry` step did), and inside translated SR writes that unmask a pending level (`Exit::Irq`). The `irq_blocked` guard is gone. Vectors are read through each CPU's own map, so Main's H-INT still comes from `$A12006`.
- Translated now: `rte`, `move to sr`, `andi/ori/eori #,sr` (`eori` previously changed only the CCR), `stop` (`Exit::Stop`: idle until an interrupt), `trap #n`, `chk`, divide by zero. Exceptions charge Musashi's totals (`lift::exc_cycles`). A privileged instruction in user mode stays with Musashi. The game never runs in user mode (F2 = <F2>).
- Return addresses stay 24-bit (frames carry the PC M1 pushed). The interpreter pushes `$FFFFxxxx` on Main; matching it is M3 work with cycle accounting.
- `lift_diff 8`: <N> samples (plain <n0>, SR writes <n1>, exceptions <n2>), 2000 interrupt entries, 0 failing. `SCD_LIFT_VERIFY` (3000 frames): 0 reports; the `rte` artifact is gone (the replay holds interrupts off).
- intro_skip census, translated: hle_stub <steps> (host-built glue counted as HLE: Sub `$180..$190`, `$200..$212`, `$5F76..$5FA0`; Main `$9A4`, `$FFFD06..$FFFD7E`, `$FFFDA8..$FFFDB4`), nothing else. Strict-RAM equal to the M1 build at every checkpoint (or: branch D1 outcome). First visible divergence vs the interpreter: frame <f> (M1: <F5>).
```

In `docs/ADR-003-static-recompilation.md`, replace the M2 row (line 40) with:

```markdown
| M2 | **Exceptions in host code.** Translate `rte`, `move to sr`, `stop`, `trap` and `chk`. Deliver interrupts in host code where the interpreter takes them: at the top of each dispatch step (slice start, after every exit) and inside SR writes that unmask a pending level. Push the frame, read the vector, run the translated handler. Remove the `irq_blocked` guard. Loop back-edges only matter once a run spans more than a slice (M5). | Fallback steps on the reference replays = `hle_stub` only (host-built BIOS glue counts as `hle_stub`). `lift_diff` covers the new instructions with 0 failures. |
```

In its `## Plans` section, replace the line `- M2 to M6: written when the previous milestone's census is in hand. M2's interrupt-delivery points depend on which loops M1 shows the game spending its time in.` with:

```markdown
- M2: `docs/superpowers/plans/2026-10-05-recomp-m2-exceptions.md`.
- M3 to M6: written when the previous milestone's census is in hand.
```

Append to its `## Progress` section:

```markdown
M2, <date>. Replay set: `intro_skip.replay` only, as for M1. `coverage --check --gate m2` passes: hle_stub <steps>, nothing else. Host-built BIOS glue (jump slots, the level-2 wrapper, the boot stub) still steps in Musashi, reported as `hle_stub`; M4 replaces it with host code. The M1 open decision on the 8 unmapped glue PCs is resolved that way. Strict-RAM equal to the M1 build on intro_skip (or: the D1 outcome). Return addresses stay 24-bit and cycle accounting stays base-only except exceptions; both move to M3.
```

In `CLAUDE.md`, replace

```
fall back to stepping Musashi for everything else (BIOS stubs, interrupts, anything untranslated or whose code bytes fail the per-range CRC check). Translated and interpreted paths must stay bit-identical. `lift_diff` and `SCD_LIFT_VERIFY` are the guards; the one known verify artifact is `rte`.
```

with

```
fall back to stepping Musashi for everything else (BIOS stubs and host-built BIOS glue, anything untranslated or whose code bytes fail the per-range CRC check). Interrupts are taken in host code where Musashi takes them (top of each dispatch step, and inside SR writes that unmask a level); `rte`, `trap`, `chk`, `stop` and divide-by-zero are translated (ADR-003 M2). Translated and interpreted paths must stay bit-identical. `lift_diff` and `SCD_LIFT_VERIFY` are the guards (verify replays with interrupts held off and should report nothing).
```

- [ ] **Step 6: Final checks and commit**

Run:
```bash
git status --short
```
Expected: only `mise.toml`, `docs/JOURNAL.md`, `docs/ADR-003-static-recompilation.md` and `CLAUDE.md` are modified. Nothing under `extracted/`, `work/` or `engine/build*` appears. If anything else does, stop.

```bash
git add mise.toml docs/JOURNAL.md docs/ADR-003-static-recompilation.md CLAUDE.md
git commit -m "recomp M2: coverage gate m2, journal and ADR progress (ADR-003)"
```

M2 is complete when the gate passes over the full reference replay set (each act, a shooting section, save/load, options and sound test: the same set M1 still needs). Until those replays exist, report M2 as "gate passes on intro_skip only".

---

## Self-review

**Spec coverage (ADR-003 M2 row and invariants):**
- Translate `rte`: Task 5. `move to sr`: Task 5 (plus `andi/ori/eori #,sr`). `stop`, `trap`, `chk`: Task 4 (plus divide by zero, which counted as `unsupported`).
- "Push the frame, read the vector, call the translated handler": Tasks 2 and 3 (`interrupt_entry`, `take_irq`, dispatch into `pick_entry`).
- Delivery points: decided in Task 3 from Musashi's own model (checks at `m68k_execute` start and in `m68ki_set_sr`, per-instruction checks removed). Back-edge polling is deferred to M5 and the ADR row is updated in Task 7.
- Remove `irq_blocked`: Task 3.
- Exit criterion "fallback steps = `hle_stub` only": Task 6 (glue, `--gate m2`) and Task 7 Step 2. "`lift_diff` covers the new instructions with 0 failures": Tasks 2, 4 and 5 (synthetic encodings, a sample-class check, interrupt trials).
- Invariant "`SCD_LIFT_VERIFY` nothing beyond the `rte` artifact (removed in M2)": Task 3 (interrupts held off in the replay, full SR and inactive-SP compare), checked in Tasks 3, 5 and 7.
- Invariant "no disc-derived data committed": Global Constraints. Every commit step stages explicit paths.
- The M1 open decision (8 unmapped glue PCs): Task 6, resolved as recommended.

**Decisions taken, not left open:**
- Return-address masking is not fixed in M2 (Global Constraints). Exception frames match M1's, `rte` masks before dispatch, and the gate is strict RAM against the parent.
- Cycle accounting is exact for exceptions only. The rest is M3. The handlers' first instructions on intro_skip are `$40E7` or glue jumps (Task 1), so `force_one` keeps M1's cycle counts.
- Privilege violations stay in Musashi. Task 1 D2 checks the assumption.

**Placeholder scan:** the only `<...>` fields are values that experiments produce (journal and ADR numbers in Tasks 1 and 7), as in the M1 plan. Every code step has its code.

**Type consistency:** `exception_entry`/`interrupt_entry`/`set_sr`/`exc_cycles`/`enter_exception`/`push_frame` (Task 2) are used under the same names in Tasks 3 to 5. So are `Exit::Irq`/`Exit::Stop` (kinds 6 and 7; the harness test in `test_gen_program.py` still sees `NotTranslated = 5`), `LoopStep::{kDeliver, kIdle, kRun}`, `loop_step(force_one, stopped, ipl, sr)`, `lift_load`/`lift_store`/`take_irq`/`stopped_`, `Insn.sr_write`, flow `('exc',)`, `PRIV`, `Case.kind` (0, 1, 2), and `host_glue`/`HOST_GLUE`/`GATES`/`--gate`.

**Review Focus:** each line names its test in the owning task: Task 2 `irq_trials`; Task 3 `loop_step` and `test_interrupts_enter_translated_handlers`; Task 4 `trap`/`stop` synthetic cases and the harness test; Task 5 SR-write cases with pending levels.
