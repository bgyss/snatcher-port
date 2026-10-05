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
| M2 | **Exceptions in host code.** Translate `rte`, `move to sr`, `stop`, `trap` and `chk`. Deliver interrupts at block boundaries and loop back-edges: push the frame, read the vector, call the translated handler. Remove the `irq_blocked` guard. | Fallback steps on the reference replays = `hle_stub` only. `lift_diff` covers the new instructions with 0 failures. |
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
- M2 to M6: written when the previous milestone's census is in hand. M2's interrupt-delivery points depend on which loops M1 shows the game spending its time in.

## Progress

M1, 2026-10-05. Replay set: `tools/playtest/replays/intro_skip.replay` only (60000 frames, reaches the first in-game screen). Gate passes on intro_skip for disc code only; M1 is not complete until the replays for every act, a shooting section, save/load and options/sound test exist and `mise run coverage` passes over all of them.

| intro_skip, translated build | not_translated | code_mismatch | irq_entry | hle_stub | unsupported |
|---|---|---|---|---|---|
| before seeds | 1888349 steps / 798 pcs | 12 / 2 pcs | 1345692 | 123243 | 0 |
| after `mise run census` + `mise run coverage` | 478615 / 8 pcs | 0 | 1345684 | 123249 | 0 |

- Overlay images seen: `ovl_936869c7` (the `ovl_909.bin` dump), `ovl_a7b6f72d`. Both are translated from their census dumps. The 2 code_mismatch PCs before seeds were the ovl_909 program claiming PCs inside `ovl_a7b6f72d`.
- `mainram` (`$FFE100..$FFE3D0`): no interpreter steps after seeding (`$FFE106` was the one not_translated PC), no code_mismatch on Main.
- The 8 remaining PCs are host-built BIOS glue at addresses no program covers (`unmapped:sub` 7: `$180..$18E` level-2 wrapper, `$5F7C` jump slot, `$200`/`$206` boot stub; `unmapped:main` 1: `$FFFDAE` jump slot to the `_BURAM` trap). `coverage --check` fails on them until they are either reported as `hle_stub` or translated (open decision).
