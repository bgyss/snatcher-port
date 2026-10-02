# snatcher-decomp

A research project to reverse-engineer Konami's *Snatcher* (Sega CD, 1994) and
build:

1. a clean-room **PC port**: a new engine that runs the original game data from
   your own disc;
2. a **remaster** that also supports the PC Engine CD (Japanese) version, a new
   English translation of it, and mixed text and voice language options;
3. a separate **UE5 third-person reimagining** prototype.

**No game data is in this repository.** Supply your own disc images in
`sega-cd-eng/` and `pcengine-cd-jpn/`. Both folders are gitignored.

## Docs

- [`docs/STRATEGY.md`](docs/STRATEGY.md): approach, phases, legal guardrails
- [`docs/DISC_LAYOUT.md`](docs/DISC_LAYOUT.md): measured disc and format findings
- [`docs/SKILLS_AND_TOOLS.md`](docs/SKILLS_AND_TOOLS.md): agent skills, tools, prior art
- [`docs/JOURNAL.md`](docs/JOURNAL.md): RE log

## Tools

```sh
# Sega CD: header + ISO9660 listing (add --extract extracted/scd to dump files locally)
python3 tools/disc_inspect.py "sega-cd-eng/Snatcher (Sega CD) (U)-redump.bin"
```
