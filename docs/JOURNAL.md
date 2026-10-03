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

## 2026-10-03: content formats (branch `content-extraction`, `tools/content/`)

Method: `SCD_PROBE=<M|S>:<pc>,...` logs registers (+16 bytes at a5) whenever the interpreter reaches a PC; decoders are
accepted only when they reproduce what the game's own routine did.

- **Konami LZ** (Main `$FF1996`, Sub `$F348`; `$F346` skips a 2-byte header first): 8 flags per byte, LSB first; flag 0 =
  literal; flag 1 = byte b: `1F` end, `<80` 10-bit offset `((b>>5)&3)<<8|next`, length `(b&1F)+3`; `80..BF` offset
  `b&0F`, length `(b>>4)-6`; `>=C0` raw run of `b-B8` bytes. 171/171 blobs the Sub CPU decompressed during the intro
  are byte-identical in `tools/content/konami_lz.py` (located on disc by signature with `lz_locate.py`).
- **DATA packs** load at PRG `$28000`. Command lists `FFFF <type> ...`: `01xx` = graphics `(u32 ptr, u16 0, u16 VRAM
  addr)` entries, each ptr -> `[u16 decompressed size][LZ]`; tiles from `0x0000`, 32x32 nametables at `0xC000`
  (plane A) / `0xE000` (plane B). `000A` = palette `<u16 ?> <u16 count-1> <u32 ptr>`: the Neo Kobe City palette at
  `DATA_D0+0x36F60` equals CRAM lines 0-1 at frame 13,800 (lines 2-3 follow it, slightly animated). `02xx` = sprite
  objects `(u16 id, i16 x, i16 y, u32 data, u16 frame)`. D packs contain 68000 code; the intro does not run the script VM.
- **Scripts** `SPxx.BIN` load at PRG `$1A800` (SP06 seen at Junker HQ). Bytecode from 0, `FF` padding, text table at
  `0x3800`: `FF`-terminated ASCII, `F2` newline, `F6` new box?, `F4` auto-advance?, `FA/FB/FC/FE`..`F9` highlight
  colours, `EC/EE` button glyphs.
- **Script VM**: dispatch `$16974` (opcode = byte at a2+a3, a2 = script base, a3 = pc), table `$16CDC` (4 bytes per
  opcode, 0x00-0x4A): byte 0 / high nibble / low nibble of byte 1 = types of operands 1-3, word 2 = handler - `$15400`.
  Operand types (from the skip routine `$1699A`): 0/7 none, 4 nested instruction, else u16; a type with bit 3 makes a
  block whose u16 after the opcode is its end address. All 39 scripts decode exactly to the padding, with a bare `00`
  before it as the end. `op43(op20(speaker, text))` shows a line (8,808/8,808 text offsets resolve), `op38(label,
  body)` is a menu option (3,755/3,755 labels resolve), `op26` groups options, `op25` = sequence, `op2B` = goto.
- **PCM**: `PCMLD_01` is 8-bit sign-magnitude. 35 clips have `[u32 len][FFFFFFFF][u16 Hz][6x00]` headers (12-32 kHz);
  the rest are streams indexed by `PCMLT_01` records `<u16 kind> <u16 param> <u16 start sector> <u16 sectors>` (kinds
  `03xx/04xx/07xx/08xx`, plus a trailing table of 8-word records). During the intro the game only writes FD `0x0600`
  (32,552 x 0x600/0x800 ≈ 24,414 Hz).
- Correction: the 2026-10-02 frame-time and translated-vs-interpreter comparisons passed presses as an unquoted zsh
  `$P`, which zsh does not word-split, so those runs had **no input** (boot to the title only). Same-input comparisons
  remain valid; coverage was narrower than reported.
