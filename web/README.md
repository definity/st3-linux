# st3 in the browser (WebAssembly)

The same emulator core as the Linux version (`../st3emu/src`), compiled to WebAssembly and running the original
Scream Tracker 3.21 in Chrome (or any modern browser).

## Build and run

```sh
source /path/to/emsdk/emsdk_env.sh   # Emscripten: https://emscripten.org/docs/getting_started/
make serve                           # builds into build/ and serves it at http://localhost:8321
```

Any static web server works: the page needs to be served over http(s), not opened as a file.

## How it fits together

| File | Runs on | Job |
|---|---|---|
| `st3web.c` | worker | thin C wrapper around the emulator core: init, run, render, keys, audio |
| `st3-worker.js` | worker | runs the emulator on its own thread; keeps `C:\ST3` in IndexedDB |
| `audio-worklet.js` | audio thread | plays the 48 kHz output, fed straight from the worker |
| `st3.js` | page | keyboard (browser keys → PC BIOS key codes), drawing, file upload and download |
| `index.html` | page | the page |
| `webdrive.mjs` | test | drives the page in headless Chrome: real key events, screenshots, timing and audio stats |

The emulator lives on a worker so that page work (drawing, saving) can never starve the audio. The audio device is
the clock: the worker keeps about 0.1 s queued in the player.

## Notes

- **Files:** ST3's `C:\ST3` drive is stored in the browser (IndexedDB), seeded with the bundled songs and
  instruments. Songs you save survive reloads. *Add songs* puts files in `C:\ST3\S`, and *Download files* gets them out.
- **Keys:** F-keys work normally. `Ctrl-F` then `1`…`0` gives F1…F10 for keyboards whose top row does other
  things (Chromebooks). Chrome keeps Ctrl-W, Ctrl-T and Ctrl-N for itself, except in **Full screen**, where the page
  asks for keyboard lock. Because ST3 uses Ctrl-W for "Save As", the page asks before it is closed.
- **Sound** starts on the first click or key press (browsers require a user gesture).
