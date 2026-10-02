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
