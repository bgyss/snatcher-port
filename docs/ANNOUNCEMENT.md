# Announcement posts (draft)

> **Status: still testing.** The game boots and plays through the intro into the first in-game scene, but I am
> still testing the rest of the game. The repo stays private until testing is done; then I will make it public and
> post the link. Every post below says so, so nobody goes looking for a link that doesn't exist yet.

Facts the posts rely on (all from this repo, so they stay accurate):

- *Snatcher* (Konami, Sega CD, 1994, written and directed by Hideo Kojima), English US release.
- A Mega CD core runs the game's own 68000 code. The Sega CD BIOS is emulated in host code (HLE), so **no BIOS file is
  needed**. CPUs: Musashi; FM synth: Nuked-OPN2. VDP, RF5C164 PCM, PSG, CD drive and Gate Array are new code.
- A build-time translator turns the game's 68000 programs (Main CPU, Sub CPU engine and the `$28000` overlay) into C++
  and compiles them into the binary, falling back to the interpreter for anything it can't translate. It is
  differential-tested against Musashi: 10,425 instruction encodings, ~83,000 random-state samples, **0 mismatches**.
- MAME is the ground truth. The title screen matches MAME **100% pixel for pixel**.
- Native SDL3 builds for macOS and Windows. Konami Justifier light gun supported (the mouse is the gun).
- **Bring your own disc**: the build reads code and assets from your own copy. No game data, BIOS or derived code is in
  the repo.
- Emulating a frame takes ~1.7 ms on an M1 Max in the headless benchmark (~3.6 ms average inside the windowed app),
  of a 16.7 ms budget. Pacing locks to vsync; tested on a 120 Hz ProMotion panel only (60 Hz is supported but untested).
- The demo video comes straight from the engine's own recorder (frame-exact, not a screen capture).

## Videos to attach

Generated locally in `work/demo/`, which is gitignored; the footage comes from the game and is never committed. Both are
1440x1080 (4:3) at 60 fps, H.264 + AAC, recorded from the engine's own frames with no screen capture.

| File | Length | Size | Use for |
|---|---|---|---|
| `work/demo/snatcher-intro-x.mp4` | 2:16 | 22 MB | **X/Twitter** (2:20 limit without Premium). Dedication card to the SNATCHER logo |
| `work/demo/snatcher-intro-full.mp4` | 6:58 | 63 MB | **LinkedIn, Reddit**, YouTube, or X with Premium. Konami logo through the whole intro into the first in-game scene |

To regenerate (needs your disc and ffmpeg):

```sh
engine/build/scd_headless "$SCD_CUE" --frames 25560 --out work/demo \
  --press 1700:S:5 --press 1900:S:5 --press 2100:C:5 --press 2300:C:5 --video work/demo/intro_master.mp4
ffmpeg -ss 7 -t 418 -i work/demo/intro_master.mp4 -vf "fade=in:st=0:d=0.5,fade=out:st=415.5:d=2.5" \
  -af "afade=in:st=0:d=0.5,afade=out:st=415.5:d=2.5" -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p \
  -c:a aac -b:a 192k -movflags +faststart work/demo/snatcher-intro-full.mp4
ffmpeg -ss 55 -t 136 -i work/demo/intro_master.mp4 -vf "fade=in:st=0:d=0.3,fade=out:st=134:d=2" \
  -af "afade=in:st=0:d=0.3,afade=out:st=134:d=2" -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p \
  -c:a aac -b:a 192k -movflags +faststart work/demo/snatcher-intro-x.mp4
```

(The presses: Start opens Options, which a fresh save forces on first run; Start then C, C leave it and start the
story.) To record your own gameplay instead, run `snatcher <cue> --record clip.mp4`, or press F9 in game.

---

## X / Twitter (thread, attach `snatcher-intro-x.mp4` to post 1)

**1/4**

> I've been reverse-engineering Konami's Snatcher (Sega CD, 1994) into a native Mac/Windows build. It runs the game's own 68000 code, and the Sega CD BIOS is emulated, so no BIOS file is needed.
>
> Here's the intro, recorded straight from the engine 🎥

**2/4**

> How it works: a build-time translator turns the game's 68000 programs into C++ and compiles them in. It's tested instruction by instruction against the Musashi interpreter: 10,425 encodings, 0 mismatches. MAME is the reference, and the title screen matches it pixel for pixel.

**3/4**

> Native SDL3 front-end with vsync-locked pacing (smooth on a 120 Hz MacBook), and a frame takes ~2 ms to emulate on an M1 Max. Konami Justifier light gun works with the mouse.
>
> Bring your own disc: no game data, BIOS or derived code ships with it.

**4/4**

> Still testing the rest of the game. The repo goes public when I'm done, and I'll post it here.
>
> Snatcher © Konami. A fan project, not affiliated with Konami. #Snatcher #SegaCD #ReverseEngineering #RetroGaming

*Single-post version* (if you don't want a thread):

> I'm reverse-engineering Konami's Snatcher (Sega CD, 1994) into a native Mac/Windows build: it runs the game's own 68000 code, translated to C++ at build time, with the BIOS emulated. Bring your own disc. Still testing; the repo goes public when I'm done. 🎥 #Snatcher #SegaCD

---

## LinkedIn (attach `snatcher-intro-full.mp4`)

> **Bringing Snatcher (Sega CD, 1994) to modern Macs and PCs: a progress update**
>
> For the past while I've been working on a side project: a native macOS and Windows build of Hideo Kojima's cyberpunk
> adventure *Snatcher* (Konami, Sega CD, 1994), made by reverse-engineering the original software. The video is the
> game's full intro, recorded straight from the engine.
>
> A few parts I'm proud of:
>
> 🔹 **It runs the game's own code.** A Mega CD core runs the original 68000 programs for both of the console's CPUs.
> The Sega CD BIOS is reimplemented in host code, so no BIOS dump is needed.
>
> 🔹 **Build-time binary translation.** A translator decodes the game's 68000 machine code and emits C++, which is
> compiled into the app. Anything it can't translate safely falls back to an interpreter, so the build always works
> and coverage grows over time.
>
> 🔹 **Differential testing.** Every distinct instruction encoding in the game (10,425 of them, about 83,000
> randomized test states) runs through both the translated code and the reference Musashi interpreter, comparing
> registers, flags and memory. The current result is zero mismatches. Whole scenes are checked against MAME, and the
> title screen matches it pixel for pixel.
>
> 🔹 **Profiling before optimizing.** Fixing frame stutter on a 120 Hz MacBook turned out not to be about emulation
> speed (a frame takes under 2 ms to emulate). The real causes were a macOS Metal presentation stall I could reproduce without the
> game at all, and a frame-pacing scheme that drifted against the display's refresh. Locking to vsync and resampling
> audio to absorb clock drift made it smooth.
>
> 🔹 **Bring your own disc.** The build reads code and assets from the player's own copy. No game data, BIOS or
> code derived from them is distributed.
>
> I'm still testing the rest of the game. When that's done I'll open-source the repo and share it here.
>
> *Snatcher is © Konami. This is a non-commercial fan project, not affiliated with or endorsed by Konami.*
>
> #ReverseEngineering #Emulation #RetroGaming #CPlusPlus #SoftwareEngineering #GamePreservation

---

## Reddit (attach `snatcher-intro-full.mp4`, or upload it to YouTube and link it)

Suggested subreddits: r/SegaCD, r/emulation, r/ReverseEngineering, r/retrogaming. Check each one's self-promotion
and flair rules before posting; r/emulation and r/ReverseEngineering prefer technical write-ups, and the post below is
written for them. For r/SegaCD / r/retrogaming, the first two paragraphs plus the "status" paragraph are enough.

**Title:** I'm reverse-engineering Snatcher (Sega CD) into a native Mac/Windows build. Here's the full intro running on it

**Body:**

> Hi all! I've been working on a native port of Konami's **Snatcher** (Sega CD, 1994, US English release) and wanted
> to share progress. The video is the full intro (Konami logo through the opening credits into the first in-game
> scene), recorded frame-exact from the engine's own output.
>
> **What it is**
>
> - A Mega CD core that runs the **game's own 68000 code** for both the Main and Sub CPUs. I didn't hand-port it.
> - The **Sega CD BIOS is emulated in host code** (HLE: traps at the BIOS entry points call C++), so no BIOS file is
>   needed. Most of the bugs along the way were in HLE return values (CDBSTAT time and status fields, BURAM) and
>   timing (V-INT relative to the VBLANK flag, CD read speed, `_WAITVSYNC` parking the Sub CPU, DMA stalls, Word RAM
>   DMA being one word late).
> - A **build-time 68000 to C++ translator** for the Main IP, the Sub CPU engine (`SUBCODE.BIN`) and the overlay at
>   `$28000`. It does recursive descent from entry points and emits one `switch(pc)` function per program. Each code
>   range is checked by CRC at runtime, and anything untranslated or modified in RAM drops back to Musashi.
> - **Differential testing:** all 10,425 instruction encodings found in the game's code (about 83k random-state
>   samples) are run in both the translator and Musashi. Registers, CCR, PC and memory have to match, and right now
>   there are 0 mismatches.
> - **MAME is the oracle.** Lua scripts dump screens, RAM and VDP writes on matching frame numbers. The title screen is
>   100% pixel-identical to MAME.
> - **SDL3 front-end** for macOS and Windows, vsync-locked pacing (tested at 120 Hz), audio drift absorbed by
>   resampling, and **Konami Justifier** support with the mouse as the light gun. YM2612 is Nuked-OPN2.
>
> **What it isn't**
>
> - It includes no game data. **You need your own disc**: the build reads code and assets from your own image, and
>   nothing derived from the disc goes in the repo or the binaries I share.
> - It isn't a byte-matching decomp. The original is almost certainly hand-written 68000 assembly, so there's no C to
>   match. The longer-term plan is a data-driven reimplementation that uses this work as the spec and the emulated core
>   as the oracle.
>
> **Status:** the intro and the start of the game run with audio, but I'm still testing the rest of the game. **The
> repo isn't public yet.** I'll publish it once testing is done and update this post with the link.
>
> Happy to answer questions about the Sega CD internals, the BIOS HLE or the translator.
>
> *Snatcher is © Konami. Non-commercial fan project, not affiliated with Konami.*

---

## Checklist before posting

- [ ] Watch both videos through once (audio on).
- [ ] X: attach `snatcher-intro-x.mp4` (2:16). LinkedIn/Reddit: `snatcher-intro-full.mp4` (6:58).
- [ ] Don't link or attach anything from `dist/`: those binaries embed code derived from the disc.
- [ ] When the repo goes public: reply to the X thread, edit the Reddit post, comment on LinkedIn with the link.
