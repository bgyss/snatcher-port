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
