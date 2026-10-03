# snatcher-decomp

A research project to reverse-engineer Konami's *Snatcher* (Sega CD, 1994) and
build:

1. a clean-room **PC port**: a new engine that runs the original game data from
   your own disc;
2. a **remaster** that also supports the PC Engine CD (Japanese) version, a new
   English translation of it, and mixed text and voice language options;
3. a separate **UE5 third-person reimagining** prototype.

**No game data is in this repository.** Supply your own disc images in
`sega-cd-eng/` and `pcengine-cd-jpn/`. Both folders are gitignored.

## Docs

- [`docs/STRATEGY.md`](docs/STRATEGY.md): approach, phases, legal guardrails
- [`docs/DISC_LAYOUT.md`](docs/DISC_LAYOUT.md): measured disc and format findings
- [`docs/SKILLS_AND_TOOLS.md`](docs/SKILLS_AND_TOOLS.md): agent skills, tools, prior art
- [`docs/JOURNAL.md`](docs/JOURNAL.md): RE log

## Tools

```sh
# Sega CD: header + ISO9660 listing (add --extract extracted/scd to dump files locally)
python3 tools/disc_inspect.py "sega-cd-eng/Snatcher (Sega CD) (U)-redump.bin"
```

## Ghidra project

```sh
tools/ghidra/setup_project.sh        # extracts code blobs -> extracted/code, builds ghidra/snatcher.gpr
ghidra                               # File > Open Project > ghidra/snatcher.gpr
```

Programs: `scd_main_ip.bin` (Main CPU, `0xFF0000`) and `scd_sub_sp.bin` (Sub CPU: SP at
`0x6000` plus `SUBCODE.BIN` at `0xD400`). Re-running the script overwrites them. Once
manual renaming starts in the GUI, stop re-running it.

## Emulators (debuggers)

```sh
tools/emu/mame_debug.sh pce          # PC Engine CD, MAME debugger (needs System Card 3)
tools/emu/mame_debug.sh scd          # Sega CD, MAME debugger (needs BIOS mpr-15045b.bin in ~/mame/roms/segacd/)
open ~/Applications/Mesen.app        # Mesen 2: best PCE CD debugger
```

## Build environment

```sh
nix develop            # compilers, SDL3, Ghidra, mingw-w64, ffmpeg (flake.nix)
mise run setup         # .venv with capstone
mise run extract       # carve code + overlay dump from your own disc (sega-cd-eng/)
mise run build         # translated native build -> engine/build/snatcher
mise run package:mac   # dist/Snatcher.app
mise run package:windows
mise run run           # mouse = Konami Justifier (left trigger, right start); start without --justifier for pad only
```

Without Nix, the same tools via Homebrew work (`brew install cmake sdl3 mingw-w64`); on macOS use Apple clang
(`CC=/usr/bin/clang CXX=/usr/bin/clang++`), not a Nix-profile gcc.
