st3 — Scream Tracker 3.21 (Future Crew, 1994) for Linux terminals
====================================================================

What this is
  A single static x86-64 Linux ELF (`st3`) that contains the original Scream Tracker 3.21
  program (unpacked from its Future Crew protection + PKLITE layers) and runs it on a built-in
  emulated PC: 386 real-mode CPU, VGA text mode (80x50, ST3's custom fonts), Sound Blaster Pro with its
  OPL2 FM chip (AdLib instruments),
  16 MB of LIM 4.0 expanded memory (EMS, for big songs), BIOS and DOS. ST3's screen is drawn in your terminal with 24-bit color; sound goes to PipeWire.

Run it
  cd into this folder (it is ST3's "C:\ST3" drive) and run:   ./st3
  Make the terminal at least 80 columns x 50 rows (enlarge the window or zoom out with Ctrl -).
  ST3 autodetects the emulated "SoundBlaster Pro IO:220 IRQ:7 DMA:1".

Quick start
  any key ........ leave the title screen
  Ctrl-L ......... load a module.  Alt-G, type a directory (e.g.  S  ), Enter: change directory.
                   Arrow keys + Enter pick a file; answer Y to "Current song is about to be erased".
  F5 / F8 ........ play (opens the Info Screen) / stop          F1..F4 ..... orders / patterns / instruments / library
  F9 / Shift-F9 .. status / setup        F10 ........ quick help        Ctrl-Q, Y ... quit
  Ctrl-F, 1..0 ... same as F1..F10; Ctrl-F, Shift+1..0 = Shift-F1..F10 (e.g. Ctrl-F then 5 = F5, Ctrl-F then ( =
                   Shift-F9 setup).  For keyboards whose F-keys do other things.  Ctrl-F twice = a plain Ctrl-F.
                   (ST3 itself has no Ctrl-F command; this is st3's own addition, not shown in ST3's help.)
  Ctrl-] ......... leave the emulator immediately (emergency exit)
  Info Screen (F5): PgUp/PgDn picks the bottom window (None, Track-5/8/18 = scrolling pattern view, scopes,
                   note spectrum); Ctrl-T cycles 5/8/18-channel tracks; Left/Right scroll channels.
  Full manual: docs/ST3.DOC.txt   File formats: docs/TECH.DOC.txt

Chromebook notes
  * Top-row keys are media keys by default; hold Search (Launcher) and press the top row for F1..F10,
    or enable "Treat top-row keys as function keys" in ChromeOS keyboard settings, or use Ctrl-F, 1..0.
  * Some Ctrl shortcuts (Ctrl-W, Ctrl-N, Ctrl-T) may be grabbed by the Terminal app; check its settings.
  * Known issue: while the Terminal window has keyboard focus, the graphics picture can flicker with thin
    horizontal lines as it updates (the Terminal repaints more often when focused).  Unfocused it is clean, and
    other sixel terminals (e.g. xterm -ti vt340) show no lines.  --gfx text avoids it; --fps 10 may reduce it.

Long file names
  DOS (and so ST3) only knows 8.3 names like ARMANI.S3M.  Files with longer names show up in ST3 the way Windows 95
  showed them to DOS programs, as short aliases: "Armani Showers (Edge, 1994).s3m" appears as ARMANI~1.S3M.  Loading
  or saving the alias uses the real file, which keeps its long name.

Options
  --root DIR      use DIR as C:\ST3 instead of the current directory
  --no-ems        no expanded memory (ST3 then holds samples only in its ~200K of conventional memory)
  --no-sound      no audio          --audio CMD   custom audio sink (s16le, 48 kHz, stereo on stdin)
  --mips N        emulated CPU speed (default 25 million instructions/s)
  --gfx MODE      auto   (default) pixel-exact picture if the terminal can show sixel graphics, else text
                  pixel-exact VGA picture (640x400, ST3's own fonts, shown at 4:3) using terminal graphics:
                  sixel  (ChromeOS Terminal, foot, mlterm, xterm -ti vt340, Windows Terminal, WezTerm)
                  iterm  (iTerm2-style inline images: iTerm2, WezTerm)     kitty  (kitty, Ghostty, Konsole)
                  text   (characters + colors, works everywhere)
  --fps N         picture updates per second in --gfx modes (default 20; only changed frames are sent)
  --glyphs blocks text mode: draw every custom glyph as block graphics
  --log FILE      emulator debug log
  ST3_AUDIO_MS=N  (environment) audio kept queued ahead, default 70; 0 = none
  -- ARGS         pass switches to ST3 itself (e.g.  -- -p  to pause after startup messages)

Files here
  st3           the program        ST3.SET  ST3 settings        ########.ST3  instrument library index
  i/            sample library     s/       example songs (.S3M .STM .MOD); FMTEST.S3M = AdLib FM demo      docs/  original docs (UTF-8)

Scream Tracker 3.21 (C) 1993,1994 Sami Tammilehto / Future Crew — distributed by its authors as freeware.
