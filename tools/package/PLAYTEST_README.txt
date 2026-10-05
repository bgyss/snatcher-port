SNATCHER (Sega CD) - native Mac build, playtest
===============================================

This is a work-in-progress native port of Konami's Snatcher (Sega CD, 1994).
It runs on Apple Silicon and Intel Macs with macOS 12 or later.

No game data is included. You need your own copy of the game.


1. What you need
----------------
The US Sega CD version as a disc image: a .cue file plus its .bin file(s).
Both layouts work:
  - one .bin for the whole disc, or
  - one .bin per track (the usual Redump layout: "Track 01".."Track 21").
Keep the .bin files in the same folder as the .cue. CHD, ISO+MP3/OGG and other
formats are not supported (convert CHD back to cue/bin with "chdman extractcd").
No BIOS file is needed.


2. First launch (macOS will block it)
-------------------------------------
The app isn't notarized by Apple, so the first time macOS says it can't be
opened. To allow it:
  - macOS 15 or later: double-click it once, then open System Settings >
    Privacy & Security, scroll down and click "Open Anyway".
  - macOS 12-14: right-click (Control-click) Snatcher.app > Open > Open.
Or, in Terminal:   xattr -dr com.apple.quarantine /path/to/Snatcher.app


3. Starting the game
--------------------
Open Snatcher.app and drag your .cue file onto its window.
Or from Terminal:   open Snatcher.app --args "/path/to/Snatcher.cue"

On a first run the game opens its Options screen (fresh save). Select QUIT to
continue into the story.


4. Controls
-----------
Keyboard:  arrow keys = D-pad,  Z = A,  X = B,  C = C,  Return = Start
Gamepad:   D-pad or left stick,  A/Cross = B,  B/Circle = C,  X/Square = A,
           Start = Start  (connect it before launching)
In menus and dialogue, C selects / advances.

F11       fullscreen on/off
Esc       QUIT the app immediately (save first!)
F9        start/stop recording a video to ~/Movies (needs ffmpeg installed,
          e.g. "brew install ffmpeg"; otherwise ignore it)

Light gun sections: start from Terminal with the mouse as the Konami Justifier:
  open Snatcher.app --args "/path/to/Snatcher.cue" --justifier
(left click = trigger, right click = start)

Saves are kept in ~/Library/Application Support/snatcher-port/snatcher/bram.bin


5. Reporting problems
---------------------
Please note for anything that looks or sounds wrong, or crashes:
  - where you were in the game (scene / what you had just done),
  - what happened, and what you expected,
  - your Mac model and macOS version.
A screenshot (Cmd-Shift-4) or a short F9 recording helps a lot.
If the game freezes, goes black, or won't advance, please send the debug log. Start it with --log:
  open Snatcher.app --args "/path/to/Snatcher.cue" --log
The log is written to ~/Library/Application Support/snatcher-port/snatcher/debug.log (it is replaced each launch,
so copy it before relaunching). It records the machine/SDL/audio setup, every CD read, music start/stop, your key
presses, game-state changes and a status line every 5 seconds. Pressing F10 at the moment it hangs adds a full state
snapshot. It contains no game data.
If the picture stutters, start it from Terminal with
  SNATCHER_FPS=1 /path/to/Snatcher.app/Contents/MacOS/Snatcher "/path/to/Snatcher.cue"
and send the lines it prints.

Snatcher is (c) Konami. This is a non-commercial fan project, not affiliated
with or endorsed by Konami.
