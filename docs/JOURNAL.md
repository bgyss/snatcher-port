# RE journal

Append-only log of confirmed facts and attempts. One entry per session.

## 2026-10-02: recon

- Wrote `tools/disc_inspect.py` (raw MODE1/2352 reader, Sega CD header, ISO9660 walk/extract).
- Sega CD: IP at 0x800 (24 KB), SP at 0x6800 (6 KB), 100 ISO files.
  `SUBCODE.BIN` is Sub-CPU code. `SPxx` scripts are uncompressed ASCII with 0xFF/0xF2 control bytes.
- `DATA_*` packs: entropy ≈7, a pointer header in the 0x028000 range, and an embedded name string.
- PCE: raw image with no filesystem. The data track starts at LBA 3949; sector 1 is the IPL.
- Code-density scan (count of `RTS` = `4E75`): only IP, SP and `SUBCODE.BIN` contain significant 68000 code.
- Searched the skills registry. No retro-console RE skills exist; Ghidra and UE5 skills are listed in `SKILLS_AND_TOOLS.md`.
- Next: install a debugging emulator, set up a Ghidra project, confirm the `SUBCODE.BIN` load address, find LZKN1 in the IP.

## 2026-10-02: Ghidra project and emulators

- Installed the Ghidra skills (trailofbits `ghidra-headless` and mitsuhiko `ghidra`) and reviewed their scripts.
- Confirmed `SUBCODE.BIN` loads at Sub `0xD400`. Evidence: the SP loader literal at `0x61F2`, plus a brute-force base scan.
- Main IP: the security block at `0xFF0000` branches to user code at `0xFF0584`.
- `tools/ghidra/setup_project.sh` builds `ghidra/snatcher.gpr` with two programs:
  - `scd_main_ip.bin`: 93 functions, 76 VDP port references labelled.
  - `scd_sub_sp.bin` (SP plus SUBCODE): 182 functions, all 23 vectors resolved, Gate Array, PCM and BIOS labels.
- Exports land in `extracted/ghidra-exports/`, which is gitignored.
- Almost all `halt_baddata` sits in `subcode_vec21`, `subcode_vec16` and `FUN_0000f8c0`, which probably hold jump-table dispatch (script VM?). They are the next target.
- Installed MAME 0.288 (brew) and Mesen 2.1.1 (`~/Applications`). clownmdemu has no macOS build.
- MAME PCE CD with `syscard3.pce` boots Snatcher to the title screen, checked by a Lua-scripted RUN press and snapshot. Sega CD is blocked until the BIOS `mpr-15045b.bin` is supplied.
- The user supplied Sega CD BIOS dumps in `~/mame/roms/segacd/`. They were matched by SHA1 to MAME names and symlinked; the originals are untouched.
- First Sega CD boot stopped at Snatcher's own message "Return to control screen, initialize at option screen". Cause: MAME's fresh 8 KB BRAM was all zeros. Writing the standard BRAM footer (`SEGA_CD_ROM`/`RAM_CARTRIDGE___`, 0x7D free blocks) fixed it.
- Boot then reached the Konami logo, the title screen, Options and the RSS notice. A 75 s `-wavwrite` capture had peak −5.8 dB and RMS −28 dB with no silence gaps, so audio works. Automated runs use `-sound none` on purpose; interactive launches have sound.

## 2026-10-02: engine bring-up (Mega CD core with BIOS HLE)

Architecture decision: see `docs/ADR-001-engine-architecture.md`. Facts recovered while building `engine/`:

- **Main IP** = hardware driver. It calls two Main BIOS entry points: `jsr $364` (region licence screen; skipped) and
  `jsr $368` (a1 = V-INT handler, stored in the RAM jump slot `$FFFD08`). `jmp $28C` is "back to BIOS control panel".
  Main RAM jump slots at `$FFFD06` (V-INT), `$FFFD0C` (H-INT), `$FFFD12` (level 2) are what the BIOS vectors point to.
  `jsr $FFFDAE` is the Main-side `_BURAM` (same function numbers as Sub `$5F16`).
- **SP** (`$6000`): header offset table at `$6020` = init `$602E`, main `$60A8`, int2 `$60BE`, user `$629C`; `$602A` is the file-service entry
  (`d0` = 0 init, 1 poll, 2 find, 3 load to a1, 6 load, 9 name lookup; `a6` = work area `$A400`). It reads the ISO9660 directory itself
  through `_CDBIOS`: ROMREADN `$20`, CDCSTAT `$8A`, CDCREAD `$8B`, CDCTRN `$8C`, CDCACK `$8D`.
  CDC destination in `$FF8004` bits 2-0: 3 = Sub read, 4 = PCM RAM, 5 = PRG RAM, 7 = Word RAM; EDT = bit 7, DSR = bit 6; DMA address in `$FF800A` is `addr >> 3`.
  After loading, SP copies `$7300..$75CF` (Main-CPU code: BURAM wrapper etc.) into Word RAM `$B7800`; Main copies it to `$FFE100` and runs it.
- **Sub CPU BIOS contract used**: `_CDBIOS $5F22` (`d0`: `$08 $12 $20 $80 $81 $84 $89 $8A $8B $8C $8D`), `_BURAM $5F16` (0 init, 1 stat, 2 search, 3 read, 4 write, 5 delete, 7 dir, 8 verify),
  `_WAITVSYNC $5F10`; level 3 jump slot is patched by SUBCODE at `$5F84`. `$FF8032` interrupt mask must have level 2 enabled by default.
  Save file header passed to BRMWRITE: 11-byte name `SNATCHER_0x`, flag `$FF`, block count word `$000E` (14 x 32 bytes = the 448-byte save blob at Sub `$9C00`).
- **Timing gotcha**: the V-INT request must trail the VBLANK status flag by ~110 Main cycles. The IP polls VBLANK (`btst #3,$C00005`) in a loop that runs inside a
  nested V-INT handler; with simultaneous flag and interrupt it livelocks (checked against MAME's per-frame `e028/e022/e020` state, `tools/emu/trace_state.lua`).
- Own HLE stack area lives below `$5F00`; a CDBSTAT buffer placed inside the stack corrupted return addresses (fixed: `$5000`).
- Status: boots to the Konami logo, then to the backup-RAM device selection screen. Text on that screen is garbled (investigating).

### Title screen bit-exact (2026-10-02)

- Reaches the title screen; a headless frame at `$FFE020 == 0x600` is **100.00% pixel-identical** to MAME (levels compared after mapping MAME's DAC ramp).
  Oracle tooling: `tools/emu/dump_wordram.lua` (screen, Word RAM, Main RAM, state), `tools/emu/vdp_ctrl_log.lua` (Main CPU VDP port words).
  Taps must be installed after the BIOS hands over (it remaps the Main CPU space).
- **Word RAM DMA quirk**: a 68K->VRAM DMA whose source is Word RAM (`0x200000-0x23FFFF`) is one word late. The first word written is stale and the last source word is
  never transferred. The IP knows: it adds 2 to the source, then rewrites the first two words with `move.l -2(a4),-4(a5)` after the DMA (it picks the Word RAM variant by testing
  bit 5 of the source's high word). Implemented in `Vdp::do_dma`. Beware: a MAME savestate's VRAM array looked shifted by a word and misled the first fit.
- The Main-side `_BURAM` (`jsr $FFFDAE`) only drives the **RAM cartridge**; with no cartridge it must return carry set, `d0 = d1 = 0` for INIT and `$FFFF` for the rest.
  Returning success made the Sub overlay show the "which RAM do you use" device screen.
- A 100 KB overlay at disc LBA 909 (outside the ISO file list) is loaded to PRG RAM `$28000` after the Konami logo. It contains Sub-CPU code (backup RAM menu, title logic)
  and the compressed title graphics. Konami's LZ decompressor for Main-side graphics is at Main `$FF1996` (flag-byte LZ; literal / short back-reference / long run), driven by the
  DMA queue handler at `$FF17F2`-`$FF18E2` (queue entries of 12 bytes at `$FFBA00`: type, source, destination, length).

### Intro reached (2026-10-02)
- Reporting `CDBSTAT` honestly (status `$0100` + advancing absolute/track MSF while CD-DA plays, `$0500` idle) removed the Main-CPU crash after QUIT: the game now runs title -> options -> story text -> "Moscow: June 6, 1996" -> first in-game screen, with audio.
- Mac app: `tools/package/mac_app.sh` -> `dist/Snatcher.app` (arm64, bundles SDL3). Windows: CI workflow only (untested).
- Open: audio levels clip (YM/PCM scaling), PCM voice streaming and CD-DA unverified vs MAME, saves untested, no light-gun input.

### Intro skip and the "frozen on ACT 1" hang fixed (2026-10-04)
Playtest reports: Start during the intro left a black screen with the music still playing; every run froze on the "ACT 1 SNATCH" card and never reached the first playable scene.
Found with `scd_headless` (route: Start at the title opens Options on a fresh save, Down x3 + C = QUIT, then the intro plays by itself) and BIOS/PRG-RAM tracing. Neither was a missing library: the shareable build links SDL3 statically (only system frameworks are dynamic).
- **`_CDBIOS $03` is MSCPAUSEON, and the game relies on the status it leaves behind.** Confirmed against the real BIOS (MAME 0.289, US Sega CD BIOS; Sub BIOS dumped with `tools/emu/dump_subbios.lua`): `_CDBIOS` at `$2E1C` queues the command word at `$5AF2`; the command executor jumps through the table at `$2FD8` (index = function number: `$02` -> `$3396` sends CDD command 1 = Stop, `$03` -> `$38C6` sends CDD command 6 = Pause, `$04` -> `$38DA`); drive mode `$5802` is 2 while playing and 1 otherwise, flag `$5B42` bit 7 is set once paused/stopped, and in MAME's trace of the skip CDBSTAT goes `$0100` -> `$0500` about 30 frames after the Start press with the time fields frozen (`tools/emu/skip_probe.lua`, `cdbios_cmd_log.lua`). The game calls `$03` when a cinematic is skipped and before every overlay load (queue command `$11` at `$102C8`, helper `$E31A`) and then polls CDBSTAT until the status word is no longer `$0100` (`$D572`/`$D59C`/`$102DE`). HLE treated `$03` as a no-op, so CD-DA stayed "playing" until the track ended: the skipped intro waited out the rest of track 3 (~135 s) on a black screen with the audio still playing. Now `$03` pauses (silent, position kept, CDBSTAT `$0500`), `$04` resumes, `$02` stops, and a new play clears the pause.
- **RF5C164 address read-back layout.** The streaming driver (`$159DE`..`$15BA4`, status byte `$1511E`: bit0 start requested, bit1 playing, bit2 one-shot load) reads each channel's address counter as bytes `$FF0021+4n` (low) and `$FF0023+4n` (high) and compares it with the stream's end address; bit1 clears only on an exact match, and the cinematic runner (`$1681E` loop) waits for `$1511E & 7 == 0`. `Pcm::read` returned channel 0/1 high bytes at those offsets, so with the channels parked at the same loop address (`$0AB8`) the driver read `$0A0A`, never saw the end, and Act 1 hung. Fixed in `Pcm::read` ($FF0020+: channel = `(a>>2)&7`, bit 1 selects high). After the fix the game goes ACT 1 -> Junker HQ (Mika at the front desk, command menu LOOK/INVESTIGATE/TALK) and plays.
- Sub-side structure worth knowing: `$9940` is a command queue (type word + args, index `$9B40`), `$9B44` = "queue submitted, executor clears when done"; the executor is `$FFC4` (jump table at `$FFE2`, handlers `$10000`+), run from the Sub level-2 handler (`$D6FE`..) once per Main V-INT. `$FF802F` is a state counter for the same handler (`$D4DC` dispatch). The cinematic is an overlay state machine (`$9808` = state) driven from `$166BA`.
- The comparison translated vs interpreter already differs from frame ~250 (Konami logo fade) in the original binaries too; not caused by this work, not investigated.
- New debugging aids: `--log` / `SNATCHER_LOG=1` (app) and `SCD_LOG=file` (headless) write a persistent log (CD reads, CD-DA, pad changes, `$E022/$E06C` game-state changes, 5 s heartbeat, 30 s no-change snapshot); `F10` snapshots state; `SCD_COMMLOG=1` logs Gate Array comm register writes with PC; `SCD_PRGPOLL=addr,..` (headless) reports Sub PRG RAM byte changes per frame; `dump_state` now includes CD and PCM channel state. `--cd-speed N` scales the emulated drive; at 4x the last intro load moves only ~100 frames earlier, so drive speed was not the delay.

## 2026-10-05: playtest harness; Engineering-exit freeze

- **Freeze signature (live app, Act 1 after leaving Engineering):** `$E020` stuck at `0x4d11` from f=19700, CD idle (lba 53570, no read pending), both CPUs spinning: Main in the V-blank poll at `$FF0920` (`btst #3, VDP status`), Sub at `$D648` in SUBCODE polling `$FF8003` bit 1 (Word RAM hand-over) after `jsr $FD96`. Last input was a C press at f=19689 (`$E06C` 8 then 0). Not yet reproduced headless.
- **Why replaying the logged presses headless did not reproduce it:** the live app loads `bram.bin` (headless starts with empty backup RAM; the first CD-read sequences already differed at f=1753), and the app sets the pad once per host iteration then may run several frames, so `[pad]` log lines are only approximate. `--record-input` now records the pad inside `run_frame()` and the starting backup RAM; see `docs/PLAYTEST.md`.
- **Determinism:** two headless runs of the same replay give identical checkpoints (VRAM, Main RAM + Sub window, framebuffer) over 12000 frames.
- **Translated vs interpreter on the intro-skip replay (12000 frames, 53 checkpoints):** RAM differs from frame 1 (stale stack bytes below SP: the translated build pushes `00FF51EA` where Musashi pushes `FFFF51EA`; plus other Main RAM bytes by frame 5), and the framebuffer differs transiently at frames `0x12c`, `0x1644`, `0x189c`, `0x19c8`, `0x1af4`, `0x1d4c`, `0x1e78`, `0x1fa4`, `0x20d0` (3840 pixels in rows 17..30 at frame 300), converging in between. Same symptom as the Konami-logo-fade note above; still not investigated. `tools/playtest/run.py --compare-headless` reproduces it.

## 2026-10-05: Recomp M1 coverage census (ADR-003)
- `SCD_COVERAGE=<file>` records every Musashi step per CPU, code image and cause; `SCD_DUMP_OVERLAYS=<dir>` dumps each image executed at `$28000`, keyed by CRC32 of its first 2 KB.
- The `$FFE100` Main code equals SP `$7300..$75CF` byte for byte (checked at frame 1500) and is translated as program `mainram`.
- intro_skip, translated build, before seeds: not_translated 1888349 steps / 798 pcs (sub 682, ovl_a7b6f72d 59, main 48, mainram 1 (`$FFE106`, which recursive descent from `$FFE100` does not reach), unmapped:sub 7, unmapped:main 1), code_mismatch 12 steps / 2 pcs (ovl_a7b6f72d `$029412`, `$02941C`: the ovl_909 program claims those PCs but the bytes of this second overlay differ there), irq_entry 1345692, hle_stub 123243, unsupported 0. Replay run for 60000 frames.
- After `mise run census` + `mise run coverage` (one pass): not_translated 478615 steps / 8 pcs, code_mismatch 0, irq_entry 1345684, hle_stub 123249, unsupported 0. Every remaining PC is host-built BIOS glue from `System::build_sub_bios`/`build_main_rom`, not disc code: Sub `$180..$18E` (level-2 wrapper: movem, `jsr $60BE`, movem, rte; ~119600 steps each), Sub `$5F7C` (level-2 RAM jump slot), Sub `$200`/`$206` (boot stub), Main `$FFFDAE` (jump slot to the `_BURAM` trap at `$70EE`). No program covers them, so the `--check` gate still fails on these 8 PCs; whether to tag them `hle_stub` or translate them is open. Nothing in `$FFE100..$FFE3D0` runs in the interpreter and Main has no code_mismatch. Overlay images seen: ovl_936869c7, ovl_a7b6f72d; ovl_909 key 936869c7 (matches).
- The translator's per-program code ranges overlap where recursive descent or the `ovl909` linear scan decoded mid-instruction (ovl909: 534 of 666 ranges overlap the previous one; sub 41 of 163; ovl_936869c7 5 of 71; ovl_a7b6f72d 7 of 40). Overlay programs now merge them into disjoint ranges and exit `_run` on any transfer out of the current range, so each range is CRC-checked before its code runs. On intro_skip (60000 frames, `--strict-ram`) the result is identical to the build without the exits, and the 20000-frame census is unchanged.
- Overlay dumps are deduplicated by the CRC of their first 2 KB, so two variants sharing those 2 KB are dumped once; the runtime per-range CRC still rejects the bytes that differ (`code_mismatch`). A bump of the overlay generation on streamed data above `$28000` can re-tag the image if an overlay rewrites its own first 2 KB.
- Translated vs interpreter is not bit-identical at whole-run level, independently of M1: on the pre-M1 parent the translated build already diverges visibly at frame 300. Generated code pushes 24-bit return addresses where Musashi pushes `$FFFFxxxx`, and the emitter charges only base cycles with no dynamic adjustments (`tools/lift/emit.py`, `gen_program.py`). Translating `mainram` moves timing further: RAM differs from frame 1022 against the parent's translated build, visible by frame 1141. A cycle-exact emitter is ADR-003 M3 work.

## 2026-10-05: Recomp M2 investigation (ADR-003)
- Parent reference for M2: the M1 translated build at `c98c81f` (`work/parent-src`, not committed). It matches the M1 HEAD build at every intro_skip checkpoint (60000 frames, RAM included). Versus the interpreter build its first visible divergence is at frame 300 (`vram`, `fb`; RAM differs from frame 1).
- Exception and privileged opcodes in the translated programs (static, case-header opcodes of the generated `switch`): `main` andi_sr 4, ori_sr 19, move_to_sr 19, rte 19, div 5; `sub` ori_sr 2, move_to_sr 2, rte 1, div 23; `mainram` none; `ovl909` chk 5, move_to_sr 1, div 24; `ovl_936869c7` chk 2, move_to_sr 1; `ovl_a7b6f72d` chk 1. No `trap`, `stop` or `move usp`; `chk` only in overlay bytes decoded by the `ovl909` linear scan (data). The overlay `move_to_sr` (`$2BDB2`, opcode `$46C4`, in `ovl909` and `ovl_936869c7`) sits at a PC that the `ovl909` linear scan decoded too (whether it is reachable code was not checked).
- Interrupt probe, intro_skip, 60000 frames, translated build: 3183688 translated SR writes; 53724 of them left a pending level above the new mask (Musashi would take the interrupt inside the instruction; the M1 loop takes it at the next dispatch step). PCs: Main `$FF087E` (`andi #$F8FF,sr`) 51659, Sub `$D4AA` (`rte`) 2065. No `move (sp)+,sr` or other SR write unmasked a pending level.
- User mode: 0 observations (translated SR writes, dispatch-loop tops and every Musashi step, translated and interpreter runs). Level disagreements (`*_irq_level()` vs the IPL Musashi sees): 0.
- The handlers' first instructions after an interrupt entry (M1 census `irq_entry` PCs) are `move sr,-(sp)` (`$40E7`, no dynamic cycles) at `$FF0CCE`, `$FF0D6E`, `$FF0EFA` (H-INT handlers vectored through `$A12006`), and the RAM jump slots `$FFFD06` (Main V-INT), `$5F7C` and `$5F82` (Sub levels 2 and 3).
- SR writes (`rte`, `move to sr`, `andi/ori/eori #,sr`) now take an interrupt inside the instruction when a pending level is above the new mask (`Exit::Irq`), as Musashi does; the M1 loop took it at the next dispatch step. Decision: accept Musashi's semantics (option (a) of the M2 plan, Task 5). Effect on intro_skip (60000 frames): against the M1 parent (`c98c81f`, strict RAM) the first difference of any kind is at frame 7200 (`ram`), the first visible one at frame 7500 (`vram`, `fb`), scene `E022:0003`; against the interpreter build the first visible divergence stays at frame 300 (RAM from frame 1), unchanged from the parent. `lift_diff 8` 0 failing (1320 SR-write samples), `SCD_LIFT_VERIFY` 0 reports over 3000 frames, `tools/test_intro_skip.sh` passes.
- The strict-RAM reference `work/parent-src` is now the translated build at `5fa5778` (matches it at every intro_skip checkpoint, RAM included); the M1 reference is kept as `work/parent-src-m1`.
- `divs` of `$80000000` by -1: Musashi sets the register to 0 with Z set and N/V/C clear (`m68k_in.c` divs special case). The translator computed it as an int32 division (undefined; on arm64 it left `$80000000` with V set: 9 of 10475 lift_diff cases failed once the operands were seeded). It now copies Musashi's special case.

## 2026-10-05: Recomp M2 host-built BIOS glue; clarifications (ADR-003)
- The 8 Musashi-stepped PCs M1 left open (Sub `$180`/`$184`/`$18A`/`$18E` level-2 wrapper, `$5F7C`/`$5F82` RAM jump slots, `$200`/`$206` boot stub; Main `$FFFD06` V-INT jump slot, `$FFFDAE` `_BURAM` jump slot) are code the host builds in `System::build_sub_bios`/`build_main_rom`, not disc code. Decision: report them as `hle_stub`, not translate them. `tools/lift/coverage_report.py` maps `not_translated` rows in `HOST_GLUE` (Sub `$180..$192`, `$200..$212`, `$5F76..$5FA0`; Main `$9A4`, `$FFFD06..$FFFD7E`, `$FFFDA8..$FFFDB4`) to `hle_stub`; keep those ranges in step with `system.cpp`. `--gate m2` rejects every cause except `hle_stub`.
- intro_skip census at `61ad6c6` (60000 frames, 213 checkpoints ok): only `hle_stub`, 1019856 steps / 23 PCs, `--check --gate m2` passes. The Task 4 census (before the SR-write change) had 1019847; the level-2/3 glue counts moved by 2 or 3 and `$5F10`/`$5F12` by -2, consistent with interrupts now taken inside SR writes.
- Clarification of the previous entry: the self-check of the rebuilt reference (`work/parent-src` at `5fa5778` vs the HEAD build) shows only that the rebuild reproduces HEAD, not that either is correct. The run compares at checkpoints, so "first difference at frame 7200" against the M1 parent means the first checkpoint that differs is 7200 (RAM differs by checkpoint 7200, the previous checkpoint matched); likewise "first visible one at frame 7500".

## 2026-10-05: Recomp M2 exceptions in host code (ADR-003)
- Interrupts are taken in host code (`System::take_irq`, `lift::interrupt_entry`) where Musashi takes them: at the top of each dispatch step, with the handler's first instruction run before the next check (as M1's `irq_entry` step did), and inside translated SR writes that unmask a pending level (`Exit::Irq`). The `irq_blocked` guard is gone. Vectors are read through each CPU's own map, so Main's H-INT still comes from `$A12006`.
- Translated now: `rte`, `move to sr`, `andi/ori/eori #,sr` (`eori` previously changed only the CCR), `stop` (`Exit::Stop`: idle until an interrupt), `trap #n`, `chk`, divide by zero. Exceptions charge Musashi's totals (`lift::exc_cycles`). A privileged instruction in user mode stays with Musashi. The game never runs in user mode (F2 = 0).
- Return addresses stay 24-bit (frames carry the PC M1 pushed). The interpreter pushes `$FFFFxxxx` on Main; matching it is M3 work with cycle accounting.
- `lift_diff 8`: 90504 samples (plain 83040, SR writes 1320, exceptions 6144), 2000 interrupt entries, 0 failing (10517 cases, 1304 skipped). `SCD_LIFT_VERIFY` (3000 frames): 0 reports; the `rte` artifact is gone (the replay holds interrupts off).
- intro_skip census, translated: hle_stub 1019856 steps / 23 PCs (host-built glue counted as HLE: Sub `$180..$190`, `$200..$212`, `$5F76..$5FA0`; Main `$9A4`, `$FFFD06..$FFFD7E`, `$FFFDA8..$FFFDB4`), nothing else; `coverage_report.py --check --gate m2` exits 0. Strict RAM (D1 outcome (a)): equal at every checkpoint (60000 frames) to the re-based reference `5fa5778`; against the M1 build (`c98c81f`) RAM differs by checkpoint 7200, visible by 7500, because SR writes now take unmasked interrupts inside the instruction. First visible divergence vs the interpreter: frame 300 (M1: F5 = 300). `tools/test_intro_skip.sh` passes.
- Known gaps: no replay executes `stop`, so `Exit::Stop` and the `musashi_stop.c` mirror of a Musashi-executed `stop` are untested end to end, and `lift_diff` exempts `stop` from the cycle comparison. An `Exit::Unsupported` on a user-mode path can charge the handler's first instruction twice (not reached: no user mode; open for M3). `divs` of `$80000000` by -1 now matches Musashi (previous entry).
