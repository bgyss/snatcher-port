# Plan: extracting Snatcher's content for the remaster

Status: in progress (branch `content-extraction`, started 2026-10-03). Owner decisions are marked **[decide]**.

## Why

The remaster is a new game: a third-person, heavily scripted-camera action adventure (God of War-style staging, built
around guns, investigation and detective work rather than melee). To build it faithfully we need the original's
**content** in forms a person (and later an automated pipeline) can read:

1. **Narrative and flow**: every line of dialogue, every menu verb and topic, every branch and flag condition, the order
   of scenes and chapters, and what triggers what.
2. **Graphics**: every background, character portrait, animation frame, cut-in and UI element, with correct palettes.
3. **Audio**: all music (CD-DA and FM), voice lines and sound effects, labelled with where they are used.
4. **Gameplay mechanics**: investigation (LOOK, ASK, INVESTIGATE ...), the Junker computer, the shooting sections
   (Justifier targets and timing), save/time systems.

The engine in `engine/` runs the game but doesn't expose any of this. This plan adds an **extraction pipeline** that
turns the disc into a reviewable content bundle.

## Ground rules

- Everything extracted is copyrighted game data. It is written to `extracted/content/` (gitignored) and **never
  committed or distributed**. Only the tools, format notes and this plan go in the repo.
- **Prove every format.** Each decoder is verified against the game itself, not just eyeballed. The proof is either a
  round trip, or the oracle: probing the original routine in our emulator and requiring identical results. Example
  (done): every LZ blob the game decompressed during the intro decompresses identically in Python.
- **Every stage writes something a person can review**: PNG sheets, WAV/FLAC, Markdown/HTML screenplay, plus a
  machine-readable JSON manifest. A human approves or rejects outputs, as in the Dark Factory loop.
- **Game-agnostic where it's cheap.** Disc/ISO reading, CD-DA ripping, the probe-and-verify method, PNG/WAV writers and
  the report format are generic. Format decoders are per-game plugins. See "Generalizing" below.

## Pipeline

One entry point, `tools/content/extract.py <disc.cue> --out extracted/content`, runs these stages in order. Each stage
is idempotent and writes `extracted/content/<stage>/` plus `manifest.json` (inputs with SHA-1s, outputs, verification
status). `REPORT.md` at the top level summarizes every stage, with links to the review artifacts.

Status as of 2026-10-03 (`extracted/content/REPORT.md` shows the live result of each check):

| # | Stage | Output | Method | Verification | Status |
|---|---|---|---|---|---|
| 1 | `iso` | the 100 ISO9660 files, plus the boot IP/SP | `media.Disc` | file count / sizes from the directory | ✅ verified |
| 2 | `cdda` | 20 CD-DA tracks → FLAC, with an index | raw 2352-byte sectors → ffmpeg | lengths from the cue | ✅ verified |
| 3 | `text` | 10,747 strings from 39 scripts → JSON + Markdown per script | `snatcher_text`: text table at `0x3800`, `FF` end, `F2` newline, markup for highlight/page/glyph codes | round trip: markup → identical bytes for every string | ✅ verified |
| 4 | `gfx-raw` | 1,625 LZ blobs → `.bin` + 4bpp tile sheet + HTML gallery | `konami_lz` (Sub `$F348` / Main `$FF1996`); pack scan for `$28000`-space pointers to `[u16 size][LZ]` | oracle: 171/171 intro blobs identical to the game's own decompression; header size == output size for all | ✅ verified |
| 5 | `gfx-scenes` | 322 composed backgrounds (planes A+B, palette) + gallery | `snatcher_gfx`: scene records → VRAM image → nametables; palette from the following `000A` command | one scene matches the engine frame; **many palettes are wrong** (layout is right) | ⚠️ partial |
| 6 | `captures` | every distinct screen the game shows, as PNG, plus its CRAM (exact palettes for stage 5) and the script position | run the engine with scripted input (`--press-every`); dedupe frames | ground truth itself | todo |
| 7 | `pcm` | 1,216 WAVs, 58 minutes (voices + SFX) | RF5C164 sign-magnitude; 35 headered clips + `PCMLT_01` index records | format matches the engine's PCM chip; streamed rate (24,414 Hz from FD `0x0600`) **needs a listen test** | ⚠️ partial |
| 8 | `fm` | FM music/SFX (`FMWR_1.BIN`) → WAV per sequence | render through Nuked-OPN2 by triggering each sequence in the engine | listen; compare with a gameplay capture | todo |
| 9 | `script` | decoded bytecode of all 39 scripts (AST JSON) + a screenplay per script + speaker sheet | `snatcher_vm`: opcode table `$16CDC`, operand rules from the VM's skip routine `$1699A`; `snatcher_story` renderer | all 39 decode exactly to the padding; live trace PCs fall on decoded instructions; all 8,808 lines and 3,755 menu labels resolve | ✅ structure verified; most opcodes still unnamed |
| 10 | `story` | the narrative bundle: chapter screenplays (who says what, under which flags), branch graph, verb/topic matrix per location, shooting sequences | stages 3, 6, 7 and 9, plus named opcodes | a human reads it next to a playthrough capture | next |

Stages 1–4 are mechanical. Stages 5, 7, 8 and 9 need reverse engineering, and each starts with an oracle probe.
Stage 9 decides how good stage 10 can be: without it we get the full dialogue in file order; with it we get the real
branching structure.

## Reverse-engineering method (reused for every format)

1. Find the routine that consumes the format (a disassembly pattern, or a watchpoint on the memory it lands in).
2. `SCD_PROBE=<cpu>:<pc>,...` (new in this branch) logs the registers and 16 bytes at `a5` each time the game runs it.
   Run the engine with scripted input (`--press`) through the content of interest.
3. Locate each source operand on the disc by its byte signature (`lz_locate.py` is the template).
4. Write the Python decoder, then require that it reproduces the game's results for every call seen.
5. Generalize from the traced cases to a static scan of the whole disc, validated by a self-consistency check (for
   LZ blobs: header size == decompressed size, and nothing the traces found is missed).

## Findings so far

Confirmed facts are also in `docs/JOURNAL.md` (2026-10-03).

- **Graphics packs.** `DATA_*.BIN` load at `$28000` (Sub PRG RAM). They hold scene command lists: `FFFF <type> ...`
  records. `01xx` loads graphics as `(u32 pointer, u16 0, u16 VRAM address)` entries; each pointer targets
  `[u16 size][Konami LZ]`, decompressed by the Sub CPU (`$F346`). Tiles go from `0x0000`, nametables to
  `0xC000`/`0xE000`. `000A` loads a palette `<u16 ?> <u16 count-1> <u32 pointer>`. `02xx` places sprite objects. The
  D packs also contain 68000 code (the intro is driven by it, not by the script VM).
- **Scripts.** `SPxx.BIN` load at `$1A800`. Bytecode from 0, `FF` padding, text table at `0x3800`. The VM (dispatch
  `$16974`, table `$16CDC`, 75 opcodes) encodes each instruction as opcode + up to 3 operands: no bytes, a 16-bit
  value, or a nested instruction; block instructions carry their end address. Programs are trees: `op25` sequences,
  `op30` statements, `op26` choice blocks of `op38(label, body)` menu options, `op43(op20(speaker, text))` lines,
  `op2B` goto. Flags and conditions (`op14`, `op19`, `op3E`, `op3F`, ...) and scene/sound commands are not named yet.
- **Audio.** `PCMLD_01` is 8-bit sign-magnitude PCM: 35 clips with a `[u32 length][FFFFFFFF][u16 Hz]` header, plus
  streams indexed by `PCMLT_01` records `<u16 kind> <u16 param> <u16 start sector> <u16 sectors>`. The game programs
  FD `0x0600` (≈24.4 kHz) for streamed playback in the intro.

## Next steps (in order)

1. **Name the opcodes that drive the story.** For each frequent unnamed opcode (`op14`, `op19`, `op3F/3E`, `op0C`,
   `op0A`, `op22/23/24`, `op42`, `op46`, `op4A`), read its handler (`$15400 + offset`) and confirm with `SCD_PROBE`
   during play. The targets are flag set/test, scene/graphics load (link to the pack records), music/CD-DA track,
   voice clip, wait, and chapter change. Each named opcode makes the screenplay read better automatically.
2. **Captures stage.** An autoplayer (`--press-every`, plus menu walking) that visits every location, with CRAM, the
   script position and frame dumps. This gives exact palettes for stage 5 and voice-to-line timing for stage 7.
3. **Speakers.** Name speaker ids in `script/speakers.json` (human review), or recover the id → portrait table.
4. **Story bundle.** Chapter order (which script follows which), a branch graph per scene, and a verb/topic matrix.
5. **FM music** (`FMWR_1.BIN`) and the second `PCMLT_01` table (SFX).

## Deliverable for review (first milestone)

`extracted/content/REPORT.md`, containing:

- the dialogue corpus: every string from every script, as one Markdown file per script
- the music: 20 FLAC tracks
- graphics: a tile sheet per blob, plus an HTML gallery per pack
- a screen-capture index of the intro and the first chapter

Each item is marked *verified* (oracle or round trip) or *unverified*.

## Generalizing to other games (the factory)

The reusable parts of this work, which a future `game-content-factory` repo can hold:

| Layer | Generic | Per game |
|---|---|---|
| Media | cue/bin, ISO9660, raw sectors, CD-DA → FLAC, CHD later | — |
| Oracle | an emulator core with `PROBE` (PC → registers + memory), scripted input, frame capture, layer masks | which emulator core; which PCs to probe |
| Codecs | LZ family toolkit (LZSS/LZKN/RLE parametrized), 4bpp/8bpp tile sheets, palette formats per console, PCM formats | the exact variant and container |
| Locator | the "signature → disc offset → re-decode → compare" loop (`lz_locate.py`) | the probe log parser |
| Reports | manifest.json schema, REPORT.md, HTML galleries, screenplay layout | how a game's script maps onto speaker / line / branch |
| Agent loop | a stage contract (input, output, verification, budget); a human review queue | the stage list |

Each stage is a small, verifiable unit of work. That's what lets an agent run many cycles by itself and hand back
artifacts with a pass/fail status, instead of claims.

## Open decisions

- **[decide]** Scope of the remaster's source of truth: the US Sega CD script only, or aligned with the PC Engine CD
  Japanese script (STRATEGY.md phase 4)? This affects stage 10's format (one language column or two).
- **[decide]** Output formats for downstream tools (e.g. Ink/Yarn for dialogue, a JSON scene graph for an engine). The
  default is plain JSON + Markdown until the remaster engine is chosen.
- **[decide]** Whether to also ingest the PlayStation/Saturn versions (extra scenes, voiced content), once the Sega CD
  pipeline is done.
