# Skills and tools for this project

Survey date: 2026-10-02. Install counts come from `npx skills find` on that date.
None of the external skills below have been installed yet. Review a skill's
contents before installing it.

## Already installed (Claude Code plugins/skills)

| Skill | Use here |
|---|---|
| `universal-modder:reverse-engineering` | Core method: strings → xrefs → functions, Ghidra via MCP, "prove every format with a round trip", and the journal discipline. Used for this study. |
| `universal-modder:mashup-mods` | Its "clean reimplementation that reads the user's own game files" and "embed a decompiled game as a library" patterns match Phases 2 and 4. |
| `universal-modder:game-recon` | Produces a recon plan. Mostly aimed at PC games; Phase 0 here covers it. |
| `universal-modder:asset-pipeline` | Sprite sheets, palettes, upscaling pipelines for the remaster art layer (Phase 3). |
| `universal-modder:fal-assets` | Generating blockout 3D, textures and voice for the UE5 prototype (Phase 4). The fal MCP server failed to authenticate this session (HTTP 401), so it needs a fresh token. |
| `universal-modder:game-automation` / `showcase-video` | Driving and recording the emulator or port for oracle screenshots and demo clips. |
| `research` | Background research write-ups into `docs/`. |
| `superpowers:writing-plans`, `test-driven-development`, `systematic-debugging` | Process skills for the engine build (round-trip tests, oracle diffs). |

## Installed from the registry (2026-10-02)

| Skill | Location | Notes |
|---|---|---|
| `trailofbits/skills-curated@ghidra-headless` | `~/.agents/skills/ghidra-headless` (symlinked into `~/.claude/skills`) | Reviewed: shell wrapper + Java export scripts, no network access. **Used by `tools/ghidra/setup_project.sh`** for `ExportAll.java` |
| `mitsuhiko/agent-stuff@ghidra` | `~/.agents/skills/ghidra` | Same script set as above, with older, less hardened JSON escaping. Kept for reference; prefer the Trail of Bits copy |

Neither wrapper (`ghidra-analyze.sh`) can import a raw binary at a base address, which 68000 boot code needs. The project therefore calls `analyzeHeadless` directly with `-loader BinaryLoader -loader-baseAddr` and our `SnatcherSetup.java` pre-script.

## Candidates from the skills registry (not installed)

| Skill | Installs | Why | Install |
|---|---|---|---|
| `zhaoxuya520/reverse-skill@ghidra-reverse` | 976 | Popular but from an unknown author. Review before use | `npx skills add zhaoxuya520/reverse-skill@ghidra-reverse` |
| `wshobson/agents@binary-analysis-patterns` | 10.3K | General binary/format analysis patterns | `npx skills add wshobson/agents@binary-analysis-patterns` |
| `quodsoler/unreal-engine-skills@ue-cpp-foundations`, `ue-project-context`, `ue-editor-tools`, `ue-input-system`, `ue-testing-debugging` | ~1K each | UE5 C++ plugin and gameplay work (Phase 4) | `npx skills add quodsoler/unreal-engine-skills@<name>` |
| `sickn33/agentic-awesome-skills@unreal-engine-cpp-pro` | 1.5K | UE C++ conventions | `npx skills add sickn33/agentic-awesome-skills@unreal-engine-cpp-pro` |
| `github/awesome-copilot@game-engine` | 12.9K | General game-engine architecture | `npx skills add github/awesome-copilot@game-engine` |
| `sadnescity/psx-spx-claude-plugin` (`cdrom-file-compression` etc.) | – | PS1 formats only. Relevant only if the PS1 port of Snatcher is added later | `npx skills add https://github.com/sadnescity/psx-spx-claude-plugin --skill cdrom-file-compression` |

The registry has **no Sega CD, Mega Drive, 68000 or PC Engine specific skills**.
A project-local skill (`.claude/skills/snatcher-re/`) should be written once the
workflow settles: disc paths, Ghidra load addresses, LZKN1 tool, oracle replay.

## Native tools

| Tool | Status on this machine | Role |
|---|---|---|
| Ghidra 12.0 | ✓ installed (`~/.nix-profile/bin/ghidra`, nix store) | 68000 disassembly/decompiler (built-in `68000:BE:32`). HuC6280 needs a community 6502-family extension; to locate |
| GhidraMCP / pyghidra-mcp | not installed | Lets the agent rename, retype and follow xrefs |
| Python 3 | ✓ | `tools/` scripts |
| ffmpeg | ✓ | PCM/CD-DA → WAV/FLAC |
| RetroArch | ✓ (Genesis Plus GX core supports Sega CD and PCE CD) | Playback reference. No debugger |
| MAME 0.288 | ✓ installed (`brew install mame`) | Debugger plus Lua for Sega CD (`segacd`) and PCE CD (`pce` + System Card). Launcher: `tools/emu/mame_debug.sh scd\|pce`. PCE verified booting to the Snatcher title screen. Sega CD BIOS: US Model 1 v1.10 (`mpr-15045b.bin`, SHA1 `f4f315ad…`) is symlinked from the user's dumps in `~/mame/roms/segacd/`. `segacd` and `megacd` verify OK. The launcher formats internal BRAM on first run (`tools/emu/format_bram.py`) |
| clownmdemu | ✗ no macOS release (v1.6.12 ships Linux and Windows only) | Would need a source build. MAME covers Sega CD debugging |
| Mesen 2.1.1 | ✓ installed (`~/Applications/Mesen.app`, official Apple Silicon build, ad-hoc signed) | Best PCE CD debugger (trace logger, event viewer, memory tools). Point it at a System Card 3 image on first CD boot (`~/Documents/RetroArch/system/syscard3.pce` exists) |
| vasm / m68k toolchain | not installed | Only needed for test patches to the original |

Still to do: a GhidraMCP server for interactive agent renaming, and a Ghidra HuC6280 extension for the PCE side.

## Prior art and references

- A report of Claude Code being pointed at a Snatcher Sega CD image. It says the
  agent disassembled the Main-CPU program in the boot sector, found an **LZSS
  routine it identified as Konami LZKN1**, built a software renderer for the
  scene display lists, and later did a TypeScript port of the game logic. This
  came up through a web search, attributed to a LinkedIn post; it is not
  independently verified. It is consistent with our finding that the IP holds the
  Main-CPU program.
- SCDTools (https://github.com/classiccoding/scdtools): Sega CD VDP ↔ PNG, PCM
  conversion, ISO file updates.
- Sega CD programming FAQ (https://segaretro.org/Sega_CD_programming_FAQ_(1998-12-06)):
  memory map, Gate Array, boot process.
- Mega CD loader write-up (https://sudden-desu.net/entry/quick-intro-to-mega-cd-development-the-sonic-cd-mmd-loader/).
- jgenesis blog on Sega CD emulation details (https://jsgroth.dev/blog/posts/emulator-bugs-sega-cd-2/).
- Romhacking.net forum thread on Snatcher Sega CD hacking
  (https://www.romhacking.net/forum/index.php?topic=38563.0).
- Junker HQ (https://junkerhq.net/neokobe/sna_e.html): version differences
  between PCE and SCD, useful for script alignment.
- A PS1 English fan translation was released in September 2026
  (https://www.timeextension.com/news/2026/09/konamis-oft-maligned-playstation-port-of-snatcher-is-finally-in-english-for-what-its-worth).
  No PC Engine English translation was found.
