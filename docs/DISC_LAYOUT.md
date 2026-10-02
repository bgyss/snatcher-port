# Disc layout findings

Everything here was measured from the user's own redump images with
`tools/disc_inspect.py` (2026-10-02). Items marked **(hypothesis)** have not been
confirmed against disassembly or an emulator yet.

## Sega CD (USA), `Snatcher (Sega CD) (U)-redump.bin`

- 21 tracks: track 1 is MODE1/2352 data; tracks 2–21 are CD-DA (redbook music).
- Image is 625,192,176 bytes = 265,813 raw sectors.

### System header (sector 0)

| Field | Value |
|---|---|
| Disc ID | `SEGADISCSYSTEM` |
| Volume / system | `SEGAIPMENU` / `KONAMI R002` |
| IP (Initial Program, Main CPU) | offset `0x800`, size `0x6000` (24 KB) |
| SP (System Program, Sub CPU) | offset `0x6800`, size `0x1800` (6 KB) |
| Copyright | `(C)T-95 1994.OCT` |
| Serial | `GM T-95035 -00` |
| Region byte | `J` (despite being the US disc) |

### ISO9660 filesystem (volume `SNATCHER`, 55,364 blocks, 100 files)

| File(s) | Size | What it is | Evidence |
|---|---|---|---|
| IP (boot area) | 24 KB | **Main-CPU program**: VDP, display lists, input | 210 `RTS`, `NOP` sleds, tile/palette tables |
| SP (boot area) | 6 KB | **Sub-CPU boot loader**: ISO9660 reader, loads `SUBCODE.BIN` | strings `MAIN A014`, `FILE NOT FOUND`, `FILE SYSTEM ERROR`, full file-name table |
| `SUBCODE.BIN` | 53 KB | **Sub-CPU engine**: script VM, CD/PCM control, file loading | starts with a `JMP` vector table (`4E71 4E71 4EF9 …`), 487 `RTS`, file-name table |
| `SP00.BIN`–`SP38.BIN` | 1–52 KB each | **Scenario scripts**: bytecode with inline English text | uncompressed ASCII; see "Script text encoding" |
| `DATA_*.BIN` (A0…Y16, ~60 files) | 12–250 KB | **Per-scene graphics packs** (tiles, palettes, display lists) | entropy ≈7.0 (compressed); pointer header |
| `PCMLD_01.BIN` | 84.7 MB | Streamed PCM bank (voices, SFX) | header `…3E80…` = 16000 (Hz?) **(hypothesis)** |
| `PCMLT_01.BIN` | 17 KB | PCM index table for `PCMLD_01` **(hypothesis)** | repeating 8-byte records `0401 FFFF xxxx yyyy` |
| `PCMDRMDT.BIN` | 52 KB | PCM drum/instrument samples | same header style as `PCMLD_01` |
| `FMWR_1.BIN` | 53 KB | FM (YM2612) music/sound sequences **(hypothesis)** | table of u32 offsets `0x400, 0x41A, …` |
| `ABS.TXT` / `BIB.TXT` / `CPY.TXT` | 56–62 B | ISO metadata (Shift-JIS + "Copyright by KONAMI") | |

### Pack header (`DATA_*.BIN`)

```
00000000: 0002 8008 0002 b058 4441 5441 5f41 305f  .......XDATA_A0_
          ^ptr0      ^ptr1     ^ "DATA_A0_BIN\0"
```

The first longwords are absolute pointers in the `0x028000` range. Pointer 0
equals `0x028000 + 8`, i.e. the end of a 2-entry table. That suggests each pack is
loaded at **`0x028000`** and carries a table of absolute section pointers
**(hypothesis)**. `DATA_D0/D1/D2/Y16` have 3 entries (`…800C`). Prior community
work (see `SKILLS_AND_TOOLS.md`) identifies the compression as Konami **LZKN1**
LZSS, with a decompressor in the Main-CPU program. This still needs to be confirmed
here.

### Script text encoding (`SPxx.BIN`)

```
…\xffCONVERSATION\xffLOVE\xffHello...?\xffA...Ahem...  This is investigator\xf2Seed.\xff…
```

- Text is plain ASCII.
- `0xFF` ends a string or menu item.
- `0xF2` is a line break inside a dialogue box.
- Bytecode before the text uses frequent `0x25 xx` and `0x30 xx` lead bytes with
  16-bit big-endian operands (e.g. `2543 2000 0c00 12`), likely `opcode, args, text
  offset` **(hypothesis)**.
- Menu verbs (LOOK, ASK, INVESTIGATE…) and topics ("ABOUT GIBSON") are stored as
  strings, so the full English script can be dumped losslessly.

## PC Engine CD (Japan), `Snatcher_(NTSC-J)_[KMCD2002].iso`

- Despite the `.iso` name it is a **raw 2352-byte/sector** image (631,563,744 B =
  268,522 sectors).
- 24 tracks:
  - track 1: audio (the standard "this is a CD-ROM" warning track)
  - track 2: MODE1 data, starting at **LBA 3949** (`00:52:49`)
  - tracks 3–23: CD-DA
  - track 24: MODE1 data (`52:07:24`), probably a duplicate of the data track as
    is standard for PCE CD **(hypothesis)**
- **No filesystem.** Sector 0 of the data track is a Shift-JIS Konami copyright
  notice. Sector 1 is the standard IPL (`PC Engine CD-ROM SYSTEM`). Data is
  addressed by raw sector number from code (HuC6280), so we must recover a
  sector map from the IPL and the loader routines.
- Text is expected to be Shift-JIS or a custom kana/kanji tile index; this is
  unverified. The PCE font is likely a 12×12 or 16×16 kanji tile set in data,
  rather than the System Card's font **(hypothesis)**.

## Confirmed load addresses (used by the Ghidra project)

| Blob | CPU | Base | Evidence |
|---|---|---|---|
| disc `0x200..0x6800` (security block + IP) | Main | `0xFF0000` | BIOS convention; `bra` at `0xFF0008` → `0xFF0584` = end of US security block |
| disc `0x6800..0x8000` (SP, module `MAIN A014`) | Sub | `0x6000` | standard SP header; jump table at `0x6020` → init `0x602E`, main `0x60A8`, int2 `0x60BE`, user `0x629C` |
| `SUBCODE.BIN` | Sub | `0xD400` | SP code at `0x61F2`: `move.w #$60,d1; movea.l #$D400,a1; jsr …`. A brute-force base scan independently ranks `0xD400` first (113/194 call targets hit function boundaries vs 46 for the runner-up) |

`SUBCODE.BIN` starts with `NOP; NOP` and then 23 `JMP` vectors (targets `0xD48E`–`0x166BA`).

## Open questions (next RE targets)

1. ~~Load address of `SUBCODE.BIN`~~ → `0xD400` (confirmed).
2. LZKN1 confirmation: find the decompressor in the IP and round-trip one pack.
3. Script VM opcode table: find the dispatch jump table in `SUBCODE.BIN`. Prime suspects are `subcode_vec21`, `subcode_vec16` and `FUN_0000f8c0`, which hold almost all of Ghidra's `halt_baddata`, typical of unresolved `jmp (pc,dN)` tables.
4. How scripts call into graphics (`DATA_*`), PCM (`PCMLT` index), and CD-DA tracks.
5. PCM sample format: RF5C164 uses 8-bit sign-magnitude. Confirm the rate and loop markers.
6. PCE sector map, text encoding and script format, for alignment with the Sega CD script.
