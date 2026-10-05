# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A research and port project for Konami's *Snatcher* (Sega CD, 1994). `engine/` is a Mega CD core that runs the game's own 68000 code, with the Sega CD BIOS emulated in host code (no BIOS file needed). It has an SDL3 front-end for Mac and Windows. Strategy and decisions live in `docs/` (`STRATEGY.md`, `ADR-001`, `ADR-002`); `docs/JOURNAL.md` is the append-only log of confirmed reverse-engineering facts. Add new findings there.

## Hard rules

- **Never commit disc images, BIOS dumps, extracted data, decompiled/disassembled listings, MAME captures, or generated translated C++.** `sega-cd-eng/`, `pcengine-cd-jpn/`, `extracted/`, `work/`, `dist/` and `engine/tests/lift_cases.inc` are gitignored. Binaries in `dist/` embed disc-derived code and must not be shared.
- The user supplies their own disc in `sega-cd-eng/` (`Snatcher (Sega CD) (U)-redump.cue/.bin`). Tools read it; nothing from it enters the repo.
- The safety hook blocks `rm -rf`. Use a fresh build directory or `mv` instead of deleting.
- Always build with Apple clang on macOS (`CC=/usr/bin/clang CXX=/usr/bin/clang++`). A `~/.nix-profile` gcc fails to link against the macOS SDK. A cached CMake dir configured with the wrong compiler must be replaced, not reconfigured.

## Environment

`nix develop` (flake.nix) provides toolchain, SDL3, Ghidra, mingw-w64, ffmpeg. `mise run <task>` wraps the workflow (`mise tasks` lists them): `setup`, `extract`, `build`, `test-lift`, `package:mac`, `package:windows`, `run`. The Python env is `.venv` (capstone); the repo's tools use it (`.venv/bin/python`).

## Build, run, test

```sh
# Extract code blobs from your disc (-> extracted/code/*.bin), needed for the translated build
.venv/bin/python tools/extract_code.py "sega-cd-eng/Snatcher (Sega CD) (U)-redump.bin"

# Plain interpreter build (no disc-derived code), CMake from engine/
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake -S engine -B engine/build-interp && cmake --build engine/build-interp -j

# Translated build: generates C++ from extracted/code at build time (needs ovl_909.bin, see below)
cmake -S engine -B engine/build -DSNATCHER_TRANSLATE_DIR=$PWD/extracted/code && cmake --build engine/build -j

# Run (SDL window; --justifier makes the mouse the light gun on port 2)
engine/build/snatcher "<disc>.cue" [--justifier]

# Packages (rebuild after engine changes; both regenerate the translation from extracted/code)
tools/package/mac_app.sh              # dist/Snatcher.app
tools/package/windows_cross.sh        # dist/windows/snatcher.exe (mingw-w64, static SDL3)

# Differential test of the translator: every instruction encoding in the disc code vs Musashi
python3 tools/lift/gen_tests.py engine/tests/lift_cases.inc 20000
cmake --build engine/build --target lift_diff && engine/build/lift_diff 8     # must report 0 failing cases
```

The `$28000` overlay is compressed on disc, so its translation input is a runtime dump:
`SCD_NO_TRANSLATE=1 SCD_DUMP_PRG=1500:28000:20000:extracted/code/ovl_909.bin engine/build-interp/scd_headless "<disc>.cue" --frames 1500 --out work/out`.

`scd_headless` (no window) is the main debugging tool: `scd_headless <cue> --frames N --out DIR [--ppm-every N] [--press FRAME:BUTTONS:HOLD] [--wav f]`. Buttons are `U D L R B C A S`. Frame numbers count from the start of the run, and the game's first frame has `$FFE020 >= 1`. Useful env vars: `SCD_STOP_E020=<hex>` (stop at a game frame counter), `SCD_STATE=1`, `SCD_DEBUG=1`, `SCD_TRACE=cpu:frame:count`, `SCD_TRACE_BIOS=1`, `SCD_WATCH=<main RAM addr>`, `SCD_VDPLOG`, `SCD_VDPPORT`, `SCD_DUMP_RAM=1`, `SCD_VRAM=1`, `SCD_DUMP_AT=f1,f2`, `SCD_NO_TRANSLATE=1`, `SCD_LIFT_VERIFY=1`, `SCD_MUTE=ym,pcm,psg,cdda`, `SCD_GUN=x,y`, `SCD_BRAM_SELFTEST=1`.

`tools/test_intro_skip.sh [scd_headless]` is the end-to-end regression for the intro skip / Act 1 hang (boots, skips the intro, expects the Junker HQ scene; works with `SCD_NO_TRANSLATE=1` too). Debug aids: `--log` / `SNATCHER_LOG=1` (app) or `SCD_LOG=file` (headless) write a persistent log, `F10` snapshots state, `SCD_COMMLOG=1` logs Gate Array comm writes, `SCD_PRGPOLL=addr,..` watches Sub PRG RAM bytes. In headless runs build `--press` lists in bash (zsh does not word-split an unquoted `$VAR`, which silently drops every press).

There is no linter. Correctness is judged against MAME (below).

## Architecture

**The game is two 68000 programs, not a script VM.** The Main CPU IP (`$FF0000`, from disc `0x200..0x6800`) is a hardware driver: VDP, controllers, a YM2612 driver, and a V-INT handler that runs most of the game via state machines. The Sub CPU runs the engine: SP at `$6000` (boot loader, ISO9660 reader), `SUBCODE.BIN` at `$D400` (script/game logic, CD-DA, PCM streaming), and overlays loaded to `$28000`. They communicate through Gate Array comm registers (`$A12010+`/`$A12020+`, flags `$A1200E/F`) and Word RAM mailboxes (`$B6xxx` on the Sub side). See `docs/DISC_LAYOUT.md` and the journal for addresses.

`engine/src/scd/`: `system.cpp` is the whole machine (both CPUs scheduled in 1/4-line slices, memory maps, Gate Array, CDC/CDD and BIOS HLE, backup RAM, audio mixing). `vdp.cpp`, `pcm.cpp` (RF5C164), `psg.cpp`, `disc.cpp`. YM2612 is Nuked-OPN2, CPUs are Musashi (both under `engine/third_party/`, with local changes listed in `engine/THIRD_PARTY.md`).

**BIOS is emulated, not run.** Illegal-opcode traps (`4AFC`) placed at the BIOS entry points (`$5F22 _CDBIOS`, `$5F16 _BURAM`, `$5F10 _WAITVSYNC` on the Sub; `$364/$368/$28C/$70EE` on the Main) call host code in `System::illegal`/`cdbios`/`buram`. Many bugs have come from HLE return values (CDBSTAT status/time fields, Main-side `_BURAM` must report "no cartridge") and timing (V-INT must trail the VBLANK flag by ~110 Main cycles; CD reads take real time, 75 sectors/s plus seek; `_WAITVSYNC` parks the Sub CPU until V-blank; VDP DMA stalls the Main CPU). A DMA whose source is Word RAM is one word late (`Vdp::do_dma`). Check `docs/JOURNAL.md` before changing these.

**Build-time translator (`tools/lift/`, `engine/src/lift/`, ADR-002).** `m68k_decode.py` decodes 68000 and emits C++ against the runtime in `cpu68k.h`; `gen_program.py <outdir> <code dir> sub|main|ovl909` does recursive descent from entry points and emits one `switch(pc)` function per program. `System::run_sub_translated`/`run_main_translated` run translated code while the PC is inside it and fall back to stepping Musashi for everything else (BIOS stubs, interrupts, anything untranslated or whose code bytes fail the per-range CRC check). Translated and interpreted paths must stay bit-identical. `lift_diff` and `SCD_LIFT_VERIFY` are the guards; the one known verify artifact is `rte`.

## Oracle workflow (MAME)

MAME is the ground truth. `nix develop` provides it (`mame`, 0.289); outside the shell use `nix build --no-link --print-out-paths --inputs-from . nixpkgs#mame` (the shell also builds the mingw-w64 cross compiler from source on Apple Silicon the first time, which takes a long while). `tools/emu/skip_probe.lua` replays the intro-skip scenario (`MAME_PRESSES="2400:S:5,2700:D:3,2760:D:3,2820:D:3,3000:C:3,7000:S:4" MAME_END=7700 mame segacd -rompath ~/mame/roms -cdrm <cue> -video none -sound none -nothrottle -autoboot_script tools/emu/skip_probe.lua`) and logs the real BIOS's drive mode/status per frame; read taps on `$5F22` only catch the first calls, so use the write taps / state polling in `cdbios_cmd_log.lua` or the BIOS dump from `dump_subbios.lua` for static analysis. Lua scripts in `tools/emu/` (run via `mame segacd ... -autoboot_script`, with `-video none` if headless) dump screens, RAM, Word RAM, VDP port writes and per-frame state; `scripted_input.lua` presses pad buttons on the same game-frame numbering as `scd_headless --press`. MAME's savestate VRAM array appeared shifted by one word, so prefer screenshots and RAM dumps over it. BIOS dumps live in `~/mame/roms/segacd/` (not in the repo). Compare screenshots by pixel level after mapping MAME's DAC ramp (the title screen at `$FFE020 == 0x600` is the regression target, 100% identical).

## Packaging notes

Playing and sharing: `docs/PLAYING.md`. `tools/package/mac_app.sh --shareable` (`mise run package:mac-share`) builds the only Mac package that may be given to others: interpreter-only, static SDL, universal, macOS 12+, and it refuses to package a binary containing translated code. Cue sheets may be one `.bin` or one per track (Redump split layout).

`mac_app.sh` and `windows_cross.sh` need `extracted/code` (including `ovl_909.bin`) to enable translation; without it they produce interpreter-only builds. Windows cross builds use pregenerated Musashi tables (`-DMUSASHI_PREGEN`) and fetch SDL3 statically. On macOS the front-end prefers the OpenGL renderer because Metal presents stall intermittently (macOS 27.2 / SDL 3.4.10); `docs/MACOS_NOTES.md` has the details and `tools/macos/check_present.sh` re-checks it. Demo videos: `snatcher --record FILE.mp4` or F9 in game, `scd_headless --video FILE.mp4`, add `--video-codec h265` for H.265 (needs ffmpeg). The light gun (Konami Justifier, port 2) is detected through the game's own controller-ID probe, so it must be enabled before boot (`--justifier`).
