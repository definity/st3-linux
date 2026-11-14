# Milestone 1 — ST3.EXE unpacked  (DONE)

Deliverable: `work/ST3U.EXE` (293,856 B plain MZ, 4211 relocs, entry 2C79:0012, Borland C startup).
Reproduce: `PYTHONPATH=tools/pylib python3 tools/unpack.py ST3.EXE x && python3 -I tools/mkexe.py ST3U.EXE`

## How ST3.EXE is protected (3 layers, all defeated by plain emulation)
1. **FCP/IV stub** (last 1292 bytes of file, entry 1E80:0006, SS=CS). Entry trick `mov sp,74; ret`.
   - Hooks IVT 0-3; INT3 handler sets TF so INT1 handler runs every instruction and does `inc ax`
     => decrypt keys depend on exact single-step count (breaks debuggers/patching; emulator must deliver TF faithfully).
   - Anti-debug probes: INT13/08 (HD), INT10/0F, `push sp` 286 test, `smsw` V86 test, self-checksum of stub bytes.
   - Decrypts the WHOLE packed image in place (paragraph-wise add/sub/xor/rol/ror mixing, 8 words per paragraph),
     verifies checksum, wipes itself with 0x2E, restores IVT, `retf` into layer 2.
2. Stub copy @1E36/1E20 (more decrypt layers).
3. **PKLITE-style LZ decompressor** @3BE4 -> real image, applies relocs, `ljmp` to 2C79:0012.
Unpacked program = Borland C/C++ (c0 startup), DGROUP seg 0x3A94 (image-relative).

## What the real program asks of its environment (from first 30M insns in tools/trace.py)
- INT10: AX=0003 set mode 3; AX=1112 load 8x8 ROM font (50-line); AX=1003 blink/intensity; AX=0500 page 0
- INT21: 30 ver, 25/35 vectors, 19 drive, 47 cwd, 43/44 attr/ioctl, 3D/3E/40 file I/O, 48/4A memory, 0A input
- Port 0x3DA polling (VGA retrace) at 30C9:0034 — need VGA status register emulation (toggle bits 0/3).
## Next (Milestone 2 — emulator architecture; discuss before building)
- 8086/286 CPU core in C (Unicorn is only a Python RE aid; final ELF must be self-contained, static).
- Memory 1MB + EMS? (instrument/sample memory may use EMS: TECH.DOC says memseg 0xFxxx = EMS handle) -> need to check XMS/EMS use.
- DOS INT21 subset w/ host filesystem (case-insensitive 8.3 mapping), BIOS INT10/16/08/1A, VGA text 80x50 -> terminal via ANSI/curses, keyboard -> scancodes (INT9/ports 0x60/0x64!),
  PIT/timer (ST3 reprograms PIT for playback tick), PIC, DMA+SB DSP emulation -> PCM out via pw-play/ALSA.
