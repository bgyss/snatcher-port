# Strategy: Snatcher (Sega CD) → PC port → remaster → UE5 reimagining

Status: study / proposal, 2026-10-02. Disc facts referenced here are in
[`DISC_LAYOUT.md`](DISC_LAYOUT.md). Tooling and skills are in
[`SKILLS_AND_TOOLS.md`](SKILLS_AND_TOOLS.md).

## TL;DR

1. **Don't do a byte-matching decompilation.** Snatcher is a script-driven
   adventure. The native code is small: Main-CPU IP is 24 KB, Sub-CPU boot SP is
   6 KB, `SUBCODE.BIN` is 53 KB, about 83 KB of 68000 in all. The game *content*
   lives in data: 39 bytecode scripts, ~60 graphics packs, PCM and CD-DA. The
   high-value, low-risk path is a **ScummVM-style clean reimplementation**: a new
   engine that runs the original scripts and assets, which it reads at runtime
   from the player's own disc.
2. Use disassembly (Ghidra, 68000) as the **spec** for the engine: script VM
   opcodes, LZKN1 decompression, display lists, PCM/CD-DA control. Use an
   emulator as the **oracle** (traces and screenshots to diff against).
3. Build the engine as a **portable C++20 core plus thin front-ends**. The SDL3
   front-end is the PC port. The same core later becomes a UE5 plugin, so
   parsers and the script VM are written once.
4. The remaster adds a second **content backend** for the PC Engine CD disc and a
   **text/voice layer** that can mix languages per line.
5. The UE5 third-person version is a **new game that uses the reimplemented
   script as its narrative backbone**. It is a separate track with a separate
   legal posture (see "Legal guardrails").

## Why not matching decomp or static recompilation?

| Approach | Fit for Snatcher | Verdict |
|---|---|---|
| Matching decomp (rebuild byte-identical binaries from C) | Original was almost certainly hand-written 68000 asm, so there is no C to match. Matching gives a *Sega CD* build, not a PC build | ✗ |
| Static recompilation (68k → C, plus emulating the VDP, Gate Array, RF5C164, YM2612 and CDC) | Two CPUs in lock-step with shared Word RAM; you would rebuild most of an emulator anyway | ✗ (fallback only) |
| Annotated disassembly (Ghidra project) | Needed to understand the VM and formats | ✓ as the **spec** |
| **Clean reimplementation, data-driven** (ScummVM / OpenTyrian / Devilution-style) | Engine is a VM plus renderers. Assets load from the user's disc. Hi-res and widescreen become possible | ✓ **primary** |

## Phases

### Phase 0: Recon and lab (done or in progress)

- [x] Inventory both discs (`tools/disc_inspect.py`).
- [x] Identify code vs data on Sega CD: IP is Main CPU, SP is the boot loader,
      `SUBCODE.BIN` is the engine, `SPxx` are scripts, `DATA_*` are graphics packs.
- [x] Find the text encoding: ASCII, `0xFF` ends a string, `0xF2` is a newline.
- [ ] Set up a debugging emulator with Sega CD support and breakpoints (MAME or
      clownmdemu; see the tools doc). Confirm it boots the redump.
- [ ] Create the Ghidra project (68000 BE). Load:
      - IP in Main-CPU Work RAM (BIOS copies it to `0xFF0000`, to be verified)
      - SP at `0x6000` in Sub-CPU program RAM
      - `SUBCODE.BIN` at the address the SP loader uses

  Gitignore the project and commit only exported symbol and label files.

### Phase 1: Format reverse engineering (each item ends with a round-trip test)

Follow the reverse-engineering skill rules: read, then hypothesize, then write a
reader, **look at the output**, write a writer, and prove a round trip.

1. **LZKN1** decompressor/compressor in `tools/`. Validate that every `DATA_*`
   pack decompresses without error and re-compresses to identical output (or to a
   functionally equal stream).
2. **Graphics packs**: decode tiles, palettes (9-bit MD CRAM), nametables, sprite
   lists and display lists to PNG. Compare against emulator screenshots of the
   same scene.
3. **Script VM**: find the dispatch table in `SUBCODE.BIN` and document every
   opcode in `docs/SCRIPT_VM.md`. Write a disassembler that turns `SPxx.BIN` into
   readable `.sns` text and reassembles it byte-identically.
4. **Audio**:
   - `PCMLT_01` index → `PCMLD_01` samples → WAV (RF5C164 8-bit sign-magnitude).
   - CD-DA tracks → FLAC.
   - `FMWR_1` FM sequences: either emulate a YM2612 with a library such as Nuked-OPN2,
     or render to audio offline.
5. **Shooting sections** (Act 1 / Junker range, light-gun): document the logic.
   This is native code, not script, so it needs proper decompilation into C++.
6. **Save format** (BRAM `SNATCHER_01`).

Deliverable: `snatcher-extract` turns a user-supplied `.cue/.bin` into a local
`extracted/` folder. Nothing extracted is ever committed.

### Phase 2: PC port (`engine/`)

- **Core (`libsnatcher`, C++20, no platform deps):**
  - disc reader (cue/bin/chd)
  - LZKN1
  - asset decoders
  - script VM
  - game state and saves
  - shooting-game logic
  - a renderer *interface* (layers, tiles, sprites, palette FX)
  - an audio interface (PCM voices, CD-DA tracks, FM)
- **Front-end (`snatcher-sdl`, SDL3):**
  - window and input (keyboard, gamepad, mouse standing in for the Justifier light gun)
  - integer or bilinear scaling
  - audio mixing
  - save slots
- **Oracle testing:**
  - Record emulator traces (script PC, opcode, flags, frame) for scripted runs.
  - Replay them through the VM and diff.
  - Screenshot diffs per scene.

  This is the gate for calling a scene "ported".
- **Milestones:**
  1. Title screen.
  2. Act 1 playable end to end with text only.
  3. Voices and music.
  4. Shooting sections.
  5. Full game.
  6. Quality of life: text speed, backlog, rewind, widescreen pillarbox art.

### Phase 3: Remaster and multi-version (`content backends` + localization layer)

Goal: one engine can play the **Sega CD English**, the **PC Engine Japanese**, and
a **new English translation of the PC Engine version**. Players can pick a language
for text and voice independently.

1. **PCE backend.**
   - HuC6280 code with raw sector addressing.
   - Use Mesen 2's debugger to trace sector reads and build a sector map.
   - Decode the PCE graphics (VDC tiles/sprites) and the Japanese text encoding.
2. **Script alignment.**
   - Convert both versions' scripts into one intermediate form: scene → node →
     line ID, with speaker, text and voice clip.
   - Align PCE JP ↔ SCD EN lines automatically (scene order, speaker, voice clip
     timing), then review by hand.
   - Where content differs, mark the delta. Known examples are censored or changed
     scenes and the Sega CD's different Act 3 pacing.
3. **Localization pipeline.**
   - Export to a standard format (gettext `.po` or XLIFF) keyed by line ID.
   - Official SCD English fills the matched lines.
   - New translations cover PCE-only lines and an optional fresh full JP→EN
     retranslation track.
4. **Remaster presentation.**
   - Optional HD art layer: upscaled or repainted art, with per-asset overrides
     and the original kept as a fallback.
   - Remastered audio: FLAC CD-DA and higher-rate PCM.
   - UI scaling.

   Asset overrides load from a user directory, so the engine never ships them.

### Phase 4: UE5 third-person reimagining (`unreal/`, separate track)

- Treat the **script IR from Phase 3 as the narrative source of truth**. Export it
  to UE DataTables or a dialogue-graph asset. Commands like LOOK, INVESTIGATE and
  TALK become in-world interactions with hotspots.
- Embed `libsnatcher` as a UE plugin. It reuses the parsers, the VM (for flags and
  branching) and the save logic. UE handles camera, characters, navigation and
  cinematics.
- Prototype order:
  1. Gillian's apartment / Junker HQ as a vertical slice: walkable, talkable,
     driven by the original script logic.
  2. Turbocycle travel between locations.
  3. Shooting sections turned into third-person cover shooting.
  4. Voice in EN and JP, with subtitles from the localization layer.
- Assets would be new (modeled from scratch, or generated through the
  `universal-modder:fal-assets` pipeline for blockout). The original sprites are
  2D references only.

## Repo layout (planned)

```
docs/            strategy, disc layout, script VM spec, journal
tools/           python extractors / analyzers (no game data)
engine/          C++20 core + SDL3 front-end (Phase 2)
ghidra/          exported symbols/labels only (project files gitignored)
unreal/          UE5 project (Phase 4)
extracted/       local only, gitignored
```

## Legal guardrails

Snatcher is Konami IP and Konami enforces it. To keep the project defensible:

- **Never commit or distribute** disc images, extracted assets, decompiled
  listings, or script text. The engine reads the user's own disc at runtime
  (the ScummVM model). `.gitignore` already blocks images and `extracted/`.
- Docs may describe formats, offsets and opcodes (interoperability), but must not
  quote large spans of game text.
- New translations count as derivative works: distribute them as patch data
  keyed by line ID that only works with the user's disc, as fan-translation
  convention does. Treat this as a known risk.
- **The UE5 reimagining is a different category.** A new game built with Konami's
  characters and story cannot be distributed without a license, whatever the
  asset provenance. Plan it as a private or portfolio prototype unless a license
  is obtained. Decide this before investing in production art.

## Risks and unknowns

- The shooting-gallery code and any timing-critical Main/Sub CPU handshakes may
  need true decompilation, not just a spec.
- PCE has no filesystem, so building the sector map is slower.
- FM music: emulating the YM2612 gives fidelity but means carrying a driver
  reimplementation. Pre-rendering is easier but loses dynamic behavior.
- The prior-art claim (LZKN1, a TypeScript port) comes from secondary sources. Verify it.
