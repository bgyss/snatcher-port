# ADR-001: Run the original 68000 code under a purpose-built Mega CD core (BIOS HLE)

Date: 2026-10-02. Status: accepted. Supersedes the "script VM first" ordering in
`STRATEGY.md` Phase 2; the rest of the strategy stands.

## Finding that forced the change

`STRATEGY.md` assumed the engine was a small script VM plus data. Disassembly shows
otherwise:

- Main CPU (`IP`, 24 KB): hardware driver. VDP, controller I/O, a YM2612 sound
  driver (writes to `0xA04000`), and a mailbox consumer. It never touches the PRG-RAM window.
- Sub CPU (`SUBCODE.BIN`, 53 KB): the **game engine**. A per-frame loop at `0xD5CC`
  reads pads, runs the state machines (`0xE046`, `0xF7F0`, `0xF446`, `0xED28`,
  `0xE4B4`, `0xE846`, `0xE34E`), and posts display work to Word RAM mailboxes
  (`0xB6000`–`0xB6500`). Script interpretation, file loading and PCM streaming
  all live here, interleaved with Gate Array and BIOS calls.

There is no clean script VM to lift out first. Rewriting 53 KB of hand-written
68000 state machines by hand, before anything runs, has no oracle and no early
feedback.

## Decision

Build `engine/` as a portable C++20 **Mega CD core with BIOS high-level emulation**:

- Musashi (MIT) for both 68000s, Nuked-OPN2 (LGPL-2.1) for the YM2612.
- Own VDP, Gate Array, RF5C164 PCM, PSG and CD-DA mixer.
- The Sega CD **BIOS is not required and not shipped**. The boot ROM, `_CDBIOS` (`0x5F22`) and
  `_USERCALL` entry points are emulated in host code. Only the BIOS calls Snatcher
  uses are implemented (found by scanning for `jsr $5f22`: `0x08 0x12 0x20 0x81 0x84
  0x89 0x8A 0x8B 0x8C 0x8D`).
- The game's own `IP`, `SP` and `SUBCODE` are loaded from the user's disc at runtime
  and run unmodified. Nothing from the disc enters the repo or the binary.
- SDL3 front-end gives the PC (Windows) and Mac builds.

The RE work (Ghidra, `docs/`) continues as the specification. Pieces are then replaced by native
code where it pays off (renderer scaling, widescreen, PCM/CD-DA quality, the PCE
backend, the UE5 plugin), each verified against the running original as oracle.

## Consequences

- Fastest route to a working PC/Mac build, and bit-exact game logic.
- It is an emulation-based port, not a source-level port. That is a known trade-off, and
  the Phase 3/4 goals (PCE backend, localization, UE5) will still need the
  format-level RE from Phase 1, which continues.
- No BIOS dependency: avoids distributing or requiring a copyrighted BIOS.
