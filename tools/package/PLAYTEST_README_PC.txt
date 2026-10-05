SNATCHER (Sega CD) - native Windows / Linux build, playtest
===========================================================

This is a work-in-progress native port of Konami's Snatcher (Sega CD, 1994).
It runs on 64-bit Windows 10 or later and on 64-bit Linux (x86_64, glibc 2.35
or newer, an X11 or Wayland desktop with OpenGL).

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


2. First launch
---------------
Windows: the exe is not code-signed, so SmartScreen may say "Windows protected
your PC". Click "More info" > "Run anyway".
Linux:   make it executable if needed:   chmod +x snatcher


3. Starting the game
--------------------
Drag your .cue file onto the window, or drop it onto the program, or from a
terminal:
  Windows:  snatcher.exe "C:\path\to\Snatcher.cue"
  Linux:    ./snatcher "/path/to/Snatcher.cue"

On a first run the game opens its Options screen (fresh save). Select QUIT to
continue into the story.


4. Controls
-----------
Keyboard:  arrow keys = D-pad,  Z = A,  X = B,  C = C,  Return = Start
Gamepad:   D-pad or left stick,  A/Cross = B,  B/Circle = C,  X/Square = A,
           Start = Start  (connect it before launching)
In menus and dialogue, C selects / advances.

F11       fullscreen on/off
Esc       QUIT immediately (save first!)
F9        start/stop recording a video (needs ffmpeg on the PATH; otherwise
          ignore it)

Light gun sections: start from a terminal with the mouse as the Konami Justifier:
  snatcher "/path/to/Snatcher.cue" --justifier
(left click = trigger, right click = start)

Saves are kept in
  Windows:  %APPDATA%\snatcher-port\snatcher\bram.bin
  Linux:    ~/.local/share/snatcher-port/snatcher/bram.bin


5. Reporting problems
---------------------
Please note for anything that looks or sounds wrong, or crashes:
  - where you were in the game (scene / what you had just done),
  - what happened, and what you expected,
  - your OS version and graphics/audio setup.
A screenshot or a short F9 recording helps a lot.
If the game freezes, goes black, or won't advance, please send the debug log.
Start it with --log:
  snatcher "/path/to/Snatcher.cue" --log
The log is written next to the save (see above) as debug.log; it is replaced
each launch, so copy it before relaunching. It records the machine/SDL/audio
setup, every CD read, music start/stop, your key presses, game-state changes
and a status line every 5 seconds. Pressing F10 at the moment it hangs adds a
full state snapshot. It contains no game data.

Snatcher is (c) Konami. This is a non-commercial fan project, not affiliated
with or endorsed by Konami.
