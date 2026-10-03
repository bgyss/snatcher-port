# ADR-002: Build-time 68000 -> C++ translation of SUBCODE

Date: 2026-10-02. Status: accepted by the project owner (chosen over hand-porting and over keeping the interpreter-only build).

## Goal
Turn the Sub-CPU engine (`SUBCODE.BIN`, 53 KB at `0xD400`, plus the SP at `0x6000` and the Main IP) into compiled C++ source, so the shipped
Mac/Windows builds no longer interpret that code. `STRATEGY.md` remains the reference: Ghidra output and the journal are the specification.

## Design
- `tools/lift68k.py` reads the user's disc (never committed), finds function entry points from the Ghidra export plus the vector tables, and emits C++
  into the build directory (`engine/build*/generated/`), one function per entry, against a `Cpu68k` register struct and the existing `System` bus.
- Flags are computed lazily per instruction pair; unsupported or data-dependent instructions (computed `jmp (pc,dn)`, self-modifying RAM code such as
  the Main code copied to `$FFE100`) fall back to the Musashi interpreter. The build therefore always works and coverage grows over time.
- Verification: every translated function is run in lock-step against Musashi on traces captured from the headless runner (register and memory
  diff after each call), and against MAME screenshots for whole scenes (`tools/emu/*.lua`).
- Generated code is derived from the disc and is **never committed or distributed**; the build step needs the user's image.

## Status
Not started beyond this decision. Capstone's M68K detail output covers the common addressing modes but not MOVEM lists, scaled indexes or the
extension-word forms reliably, so the decoder needs its own instruction table (about 70 opcodes cover SUBCODE: `scan` of the disassembly lists them).

## Progress (2026-10-02)
- `tools/lift/m68k_decode.py` (decoder + C++ emitter), `tools/lift/emit.py` (flow glue), `engine/src/lift/cpu68k.h` (runtime semantics) implemented.
- `engine/tests/lift_diff.cpp` + `tools/lift/gen_tests.py`: every distinct instruction in IP, SP and SUBCODE (10,425 encodings, ~83,000 random-state samples, odd addresses and
  small values included) is executed by the translated code and by Musashi; registers, CCR, PC and memory are compared. **Current result: 0 mismatches.**
  Undefined BCD V/N flags mirror Musashi. Not covered: `rte`/`move to sr` (privilege/stack switching), traps, `stop`, `chk`, and encodings Musashi treats as illegal.
- Next: `tools/lift/gen_program.py` (recursive-descent over entry points, one `switch(pc)` dispatch function, fall-through between adjacent instructions),
  a hook in `System` that runs translated code when the Sub CPU PC is inside it and falls back to Musashi otherwise, then whole-scene comparison against MAME.

## Integration (2026-10-02)
- `tools/lift/gen_program.py` translates SP + SUBCODE (6,414 instructions reachable by recursive descent, 154 entry points) into one `switch(pc)` function.
  `cmake -DSNATCHER_TRANSLATE_DIR=<dir>` runs it at build time and defines `SNATCHER_TRANSLATED`.
- `System::run_sub_translated` runs translated code while the Sub PC is inside it and steps Musashi for everything else (BIOS stubs, overlays at `$28000`, interrupts, HLE traps).
  It checks once that the code in PRG RAM matches the translated image (CRC). `SCD_NO_TRANSLATE=1` forces the interpreter; `SCD_LIFT_VERIFY=1` re-executes every translated instruction in Musashi and reports mismatches.
- Result: title screen 100.00% pixel-identical to MAME with the Sub CPU running translated C++; the scripted run through Options, the story text and the first in-game screen matches the interpreter build.
- Not translated yet: the Main CPU IP (`$FF0000`), the `$28000` overlays and the Main code copied to `$FFE100`; these still run in Musashi. Coverage of reachable SUBCODE is partial (jump tables are heuristic).

## Main CPU (2026-10-02)
- `gen_program.py <out> <code dir> main` translates the IP at `$FF0000` (4,189 instructions, 131 entry points incl. RAM vector tables and H-INT vector words);
  `System::run_main_translated` mirrors the Sub path (word accesses stay word accesses for the VDP ports, DMA stalls are charged to the translated budget).
- Title screen still 100.00% pixel-identical to MAME and the scripted playthrough still reaches the first in-game screen with both CPUs translated.
- Windows and Mac builds regenerate both translations from `extracted/code` at build time.
