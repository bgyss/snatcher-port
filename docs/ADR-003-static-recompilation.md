# ADR-003: From AOT-translated emulator to static recompilation

Date: 2026-10-05. Status: proposed. Builds on ADR-001 (Mega CD core, BIOS HLE) and ADR-002 (build-time 68000 -> C++ translator).

## Context

The shipped translated build is accurately described as *an emulator whose CPU cores are partly compiled ahead of time*:

- Translated code is one `switch(pc)` per program over an emulated register file (`engine/src/lift/cpu68k.h`). There are no native calls or returns.
- Musashi runs at every frame. `System::run_sub_translated` / `run_main_translated` step it for anything not translated: interrupt entry
  (translated code only runs while `irq_level <= mask`), `rte`, `trap`, `stop`, `chk`, `move to sr`, the HLE BIOS stubs, overlays other than the
  intro overlay, the Main code copied to `$FFE100`, and any range whose CRC does not match the translated image.
- Coverage is found heuristically. Jump tables are pattern-matched, overlays are linear-scanned, and nothing measures what actually ran in the interpreter.
- Both CPUs are scheduled in quarter-line slices and all hardware (VDP, Gate Array, CDC/CDD, RF5C164, YM2612, PSG) is emulated at register level.

The project is criticised as "not a real recomp". The criticism is fair for the CPU side. The Snatcher source does not exist (the game was hand-written
68000 assembly, `STRATEGY.md`), so the reachable goal is a **static recompilation**: the game's code runs as compiled C++ with no CPU interpreter in the
shipped binary. This ADR defines that goal, the order of work, and how each step is proven correct.

## Decision

Adopt static recompilation as the target for the translated build ("Path A"). It is reached in milestones. Each milestone ships a working game and has
a measurable exit criterion. The interpreter build (`SCD_NO_TRANSLATE=1`, `engine/build-interp`) stays as the oracle and is never removed.

### Definition of done for "static recompilation"

1. The shipped translated binary does not link Musashi (checked with `nm`), and every 68000 instruction the game executes on the reference replays runs as compiled C++.
2. Exceptions (interrupts, `trap`, `rte`, the HLE BIOS entry points) are delivered by host code that calls translated handlers directly.
3. Subroutines are compiled as C++ functions with native call and return wherever the stack discipline is proven. A dispatcher remains only for verified computed targets.
4. The translated build's playtest checkpoints match the interpreter build at every visible checkpoint on all reference replays (`tools/playtest/run.py --compare-headless`), and the title screen at `$FFE020 == 0x600` stays 100% pixel-identical to MAME.

Register-level hardware emulation (VDP, Gate Array, sound chips) is **not** part of this definition. Keeping a VDP renderer is normal for recompilations of
consoles without a high-level graphics API. Replacing the hardware layer with semantic, game-level rendering and audio is milestone M6 and gets its own ADR.

### Milestones

| # | Milestone | Exit criterion |
|---|---|---|
| M1 | **Coverage.** Measure every interpreter step and its cause. Dump every overlay image the game executes. Translate the `$FFE100` Main RAM code. Feed observed entry points (seeds) back into the translator. | On every reference replay, fallback steps with cause `not_translated` or `code_mismatch` = 0. The remaining fallbacks are only `irq_entry`, `hle_stub` and `unsupported` (`rte`, `trap`, `stop`, `chk`, `move to sr`). |
| M2 | **Exceptions in host code.** Translate `rte`, `move to sr`, `stop`, `trap` and `chk`. Deliver interrupts in host code where the interpreter takes them: at the top of each dispatch step (slice start, after every exit) and inside SR writes that unmask a pending level. Push the frame, read the vector, run the translated handler. Remove the `irq_blocked` guard. Loop back-edges only matter once a run spans more than a slice (M5). | Fallback steps on the reference replays = `hle_stub` only (host-built BIOS glue counts as `hle_stub`). `lift_diff` covers the new instructions with 0 failures. |
| M3 | **Function recovery.** Build a call graph from the recursive descent. Classify each subroutine's stack discipline (balanced, return-address-popping, tail-jumping). Emit balanced subroutines as C++ functions, and keep the `switch(pc)` dispatcher for the rest and for computed targets. Keep cycle accounting per basic block. | Report: share of `jsr`/`bsr` sites compiled as native calls. Checkpoint equality on all replays. |
| M4 | **No interpreter at runtime.** The HLE traps (`$5F22`, `$5F16`, `$5F10`, Main `$364/$368/$28C/$70EE`) become direct host calls from translated code. Musashi links only into `scd_headless`'s interpreter mode, `lift_diff` and the interpreter build. | The shipped `snatcher` (Mac and Windows) has no `m68k_*` symbols. Done criteria 1 to 4 above hold. |
| M5 | **Scheduling.** Replace quarter-line lock-step of two CPUs with cooperative scheduling: each CPU runs until it reaches a sync point (comm register, Gate Array flag, Word RAM swap, VDP status poll, `_WAITVSYNC`). Cycle budgets are kept for CD and V-INT timing. | Checkpoint equality, plus frame time recorded before and after with `SCD_FRAMETIME`. |
| M6 | **Semantic hardware layer** (separate ADR). Render from the game's display mailboxes (`$B6xxx`) and drive PCM and CD-DA from the Sub engine's commands, not from chip registers. This is where widescreen and hi-res come from, and it overlaps with `STRATEGY.md` Phase 1. | Defined in its own ADR. |

M1 to M4 make a static recompilation by the definition above. M5 and M6 make it a port rather than a recompiled emulator. Their order can change after M4.

### Measurement

The deciding metric is the **fallback census**: `SCD_COVERAGE=<file>` writes, per CPU and per code image, every PC that Musashi executed and why
(`not_translated`, `code_mismatch`, `irq_entry`, `hle_stub`, `unsupported`, or `interpreter` when translation is off).
`tools/lift/coverage_report.py` merges censuses from several runs into one report and writes translator seed files. The milestone exit criteria are
stated in terms of this report, run over the **reference replay set** in `tools/playtest/replays/`.

Coverage is only as good as the replays. M1 therefore includes recording replays that reach every act, the shooting sections, saving and loading, and
the options and sound test. The future autonomous explorer (`docs/PLAYTEST.md`, "Later") writes the same format.

### Invariants (all milestones)

- Translated and interpreted paths stay bit-identical. `lift_diff` must report 0 failing cases. `SCD_LIFT_VERIFY` reports nothing new beyond the known `rte` replay artifact (removed in M2).
- Generated C++, overlay dumps, seed files and coverage censuses are disc-derived. They live under `extracted/`, `work/` or the build tree and are never committed or distributed. Binaries containing translated code stay private (`docs/PLAYING.md`). The `--shareable` Mac package stays interpreter-only.
- Every code range checked at runtime keeps a CRC guard until M4. At M4 the guard becomes a startup check of the disc images. A mismatch then refuses to run, because no interpreter remains to fall back to.

## Consequences

- The "recomp" claim becomes measurable. The census report is the number to publish, not an adjective.
- More disc-derived build inputs (overlay dumps and seeds) mean `mise run extract` grows a "census" step that runs the reference replays on the interpreter build.
- M2 and M3 change the generated code's structure substantially. `lift_diff` stays per-instruction, so M3 also needs a per-function differential test (run each recovered function and the dispatcher version from the same state and compare).
- The interpreter build becomes more important, not less: it is the oracle for every milestone.

## Plans

- M1: `docs/superpowers/plans/2026-10-05-recomp-m1-coverage.md`.
- M2: `docs/superpowers/plans/2026-10-05-recomp-m2-exceptions.md`.
- M3 to M6: written when the previous milestone's census is in hand.

## Progress

M1, 2026-10-05. Replay set: `tools/playtest/replays/intro_skip.replay` only (60000 frames, reaches the first in-game screen). On intro_skip, `--check` still fails only on 8 host-built BIOS glue PCs, with no disc-code gate failures. M1 is not complete until the replays for every act, a shooting section, save/load and options/sound test exist and `mise run coverage` passes over all of them.

| intro_skip, translated build | not_translated | code_mismatch | irq_entry | hle_stub | unsupported |
|---|---|---|---|---|---|
| before seeds | 1888349 steps / 798 pcs | 12 / 2 pcs | 1345692 | 123243 | 0 |
| after `mise run census` + `mise run coverage` | 478615 / 8 pcs | 0 | 1345684 | 123249 | 0 |

- Overlay images seen: `ovl_936869c7` (the `ovl_909.bin` dump), `ovl_a7b6f72d`. Both are translated from their census dumps. The 2 code_mismatch PCs before seeds were the ovl_909 program claiming PCs inside `ovl_a7b6f72d`.
- `mainram` (`$FFE100..$FFE3D0`): no interpreter steps after seeding (`$FFE106` was the one not_translated PC), no code_mismatch on Main.
- Overlay programs (`ovl909`, the census overlays, `mainram`) leave their `_run` with `NotTranslated` whenever the next PC is outside the code range being executed, so the dispatcher re-checks the target range's CRC (`pick_entry`). Before, only the entry PC's range was checked, and a branch inside `ovl909` could run its translation over `ovl_a7b6f72d` bytes that differ (`$029412`, `$02941C`). Their ranges are also merged where decodes overlap, so ranges are disjoint. `sub` and `main` output is unchanged. Bit-identical to the parent translated build on intro_skip (60000 frames, `--strict-ram`), same census, no measurable cost.
- The 8 remaining PCs are host-built BIOS glue at addresses no program covers (`unmapped:sub` 7: `$180..$18E` level-2 wrapper, `$5F7C` jump slot, `$200`/`$206` boot stub; `unmapped:main` 1: `$FFFDAE` jump slot to the `_BURAM` trap). `coverage --check` fails on them until they are either reported as `hle_stub` or translated (open decision).

M2, 2026-10-05. Replay set: `intro_skip.replay` only, as for M1, so M2 is reported as "gate passes on intro_skip only" until the full reference set exists. `coverage --check --gate m2` passes: hle_stub 1019856 steps / 23 PCs, nothing else. Host-built BIOS glue (jump slots, the level-2 wrapper, the boot stub) still steps in Musashi, reported as `hle_stub`; M4 replaces it with host code. The M1 open decision on the 8 unmapped glue PCs is resolved that way. Strict RAM is not equal to the M1 build: SR writes now take an unmasked interrupt inside the instruction, as Musashi does (decision (a) of the M2 plan), so against the M1 parent (`c98c81f`) RAM differs by checkpoint 7200 and the screen by checkpoint 7500. The strict-RAM reference was re-based on `5fa5778`, and the M2 HEAD build matches it at every intro_skip checkpoint (60000 frames). Versus the interpreter the first visible divergence stays at frame 300, as for M1. `lift_diff 8`: 0 failing cases, 0 failing interrupt entries. `SCD_LIFT_VERIFY` (3000 frames): 0 reports. Return addresses stay 24-bit and cycle accounting stays base-only except exceptions; both move to M3.

Known limitations after M2:
- The `stop` path (`Exit::Stop`, and `src/scd/musashi_stop.c` mirroring a Musashi-executed `stop`) is untested end to end: no replay executes `stop`. `lift_diff` exempts `stop` from the cycle comparison.
- An `Exit::Unsupported` on a user-mode path (privileged instruction in user mode) can charge the handler's first instruction twice. The game never runs in user mode, so this is not reached; open for M3.
- `HOST_GLUE` in `tools/lift/coverage_report.py` copies addresses from `System::build_sub_bios`/`build_main_rom` by hand.
