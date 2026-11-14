# Route A — C emulator architecture (draft, 2026-11-14)

Goal: one static Linux x86-64 ELF `st3` that embeds ST3U.EXE and runs it in the terminal with sound.

```
st3emu/
  src/cpu.c cpu.h      386 real-mode integer interpreter (no FPU, no PM). Pure: memory + io/hle callbacks.
  src/mem.c            1MB+HMA flat RAM, A20 mask.
  src/dos.c            INT21 HLE: MCB chain, PSP/env, host FS sandbox (8.3 case-insensitive), FINDFIRST, handles.
  src/bios.c           INT10/16/1A/15/2F/33/67 HLE, BDA init, IVT -> F000:nn trap stubs.
  src/vga.c            text 80x50, attr ctl, DAC, SEQ/GC, plane-2 font capture, 512-char mode, 3DA retrace @70Hz.
  src/pic_pit.c        8259 (IRR/ISR/mask/EOI) + 8254 ch0 (divisor -> IRQ0 rate), port 61.
  src/sb.c             SB Pro DSP subset + mixer reg 0E + 8237 DMA (readable cur addr/count) -> PCM ring.
  src/audio.c          host audio out (ALSA via /dev/snd, or pipe to pw-play) — pull model at DSP rate.
  src/term.c           terminal renderer + input (raw tty, ANSI truecolor; key decode -> BIOS scancodes).
  src/main.c           loop: run CPU slices, keep virtual time locked to wall clock, deliver IRQs, render @~30-70Hz.
  tests/               differential CPU fuzzer vs Unicorn (python ctypes, libst3cpu.so) + program-trace lockstep.
```

Timing model: virtual clock advances with executed instructions (configurable MIPS); main loop throttles to
wall clock so PIT (70 Hz) and SB DMA consumption are real-time. Audio device pulls PCM; DMA position derives
from audio frames consumed (so ST3's DMA-position polling sees the true play cursor).

Verification strategy per layer:
1. CPU: random single-instruction differential fuzz vs Unicorn (mask architecturally undefined flags).
2. Whole program: compare against survey.py outcomes — screens (PNG diff via same render code), WAV of ARMANI.
3. Interactive: run in terminal.
