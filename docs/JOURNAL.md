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
