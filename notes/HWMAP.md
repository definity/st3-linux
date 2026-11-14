# ST3.21 — Hardware & Services Map (Milestone 2 survey result)

Everything below was observed by running the unpacked `ST3U.EXE` under `tools/survey.py`
(Unicorn CPU + Python HLE), driving it with scripted keys through: boot → title → pattern editor →
load dialog → load ARMANI.S3M → play with Sound Blaster Pro. Evidence: `notes/survey_*.txt|log`,
`notes/screen_*.png`, `notes/ARMANI_emulated_sbpro.wav`.

This is the **spec for the C emulator** (Route A). "MUST" = observed and required.

## 1. CPU
- Program is Borland C/C++ (large/medium model), entry 2C79:0012, DGROUP 3A94 (image-relative).
- MUST: 8086 + 186/286 real-mode instructions (enter/leave, push imm, imul r,m,imm, shifts by imm, pusha/popa).
- MUST: **386 real-mode** instructions in the sound/mixer segment (24ED): 0x66 operand-size prefix (32-bit regs),
  FS/GS (push/pop fs), 0F-prefixed near Jcc, likely movzx/movsx/shld/shrd. See §1a census.
- MUST: trap flag / INT1 only for the *packed* EXE; the unpacked EXE does not need TF. (We run ST3U.EXE.)
- `smsw` was only used by the protector stub.
- No protected mode, no DPMI, no XMS observed.

### 1a. Dynamic opcode census (during playback, 6M insns)
Source: `notes/survey_census.txt` (6M insns while ARMANI.S3M plays on SB Pro).
- Hot path: mov/push/pop/add/cmp/Jcc/inc/call/ret/loop/or/lcall/les/xlat/lods/stos/shl/shr/cbw/cwd/adc/idiv/div/mul.
- 186/286: enter, leave, imul r,r/m,imm.
- **386 (operand-size 0x66), all in mixer seg 24ED**: add, mov, shl/shr imm, push/pop r32, or, xor, movzx, sub, div r32, cmp.
- Not seen: FPU (x87), protected mode, string ops w/ 0x67, 0F jcc at runtime (static sweep found some — implement anyway).
- Census caveat: "a32" entries are a heuristic false positive (0x67 as a displacement byte); capstone labels 98/99 as cwde/cdq (=cbw/cwd in 16-bit).
- Decision: implement full 386 real-mode integer ISA incl. 0x66/0x67 prefixes, FS/GS, 0F (Jcc near, movzx/movsx, setcc, shld/shrd, bt*, bsf/bsr, imul r,rm).
  Differential test against Unicorn instruction-by-instruction.

## 2. Memory
- 1 MB real-mode address space; conventional memory up to A000 (640K). ST3 reports FreeMem ~252–274K w/ module loaded.
- MUST: **DOS MCB chain** in memory. ST3 verifies every pattern/instrument segment has an `'M'` MCB at seg-1
  (check loop at linear 0x1B48C/0x1B4F4 → "CRASH WARNING ... memory allocation error detected").
- EMS optional: probes by opening device `EMMXXXX0` (+ INT 67h). Absent → "FreeEMS 0000K", works fine.
  (EMS would allow bigger modules; can add LIM 4.0 later.)
- Video RAM B8000 (text, 80x50) + VGA plane 2 font memory (see §4).

## 3. Interrupt vectors the program installs
| INT | Handler (load-rel) | Purpose |
|---|---|---|
| 00 | 24ED:5617 | divide error (Borland/sound drv) |
| 08 | 24ED:28CD | **player tick** (PIT reprogrammed to ~70 Hz) — only with sound card |
| 0F | 24ED:635F | **SB IRQ 7** handler |
| 23 | 2C79:3571 | Ctrl-Break |
| 24 | 2C79:427D | critical error |
Keyboard is **BIOS INT 16h** (AH=00/01/02); no INT 9 hook, no direct port-60 keyboard reads.

## 4. Video (VGA, text mode)
- INT 10h: AH=00 (mode 3), 02 (cursor), 05 (page), 10/03 (blink→intensity), 11/12 (8x8 font, **80x50**).
- Direct register programming (ports): 3C0 (attr ctl, flip-flop reset by 3DA read), 3C2, 3C4/3C5 (seq), 3C8/3C9 (DAC, custom palette),
  3CE/3CF (GC), **3DA polled for vertical retrace** (ST3 measures retrace period to set the 70 Hz PIT) → emulate 70 Hz retrace in virtual time.
- **Custom fonts**: uploaded to plane 2 via B800 window (SEQ2=04, SEQ4=06, GC4=02, GC5=00, GC6=0C; restore SEQ2=03, SEQ4=03, GC4=00, GC5=10, GC6=0E).
  **512-character mode**: attr fg bit 3 selects font block @plane2+0x4000 (cursive logo tiles); block 0 = ST3 UI font w/ bevel glyphs.
  Font uploaded 3–4x at startup only (static during use).
- Pixel-exact render verified: `notes/screen_title.png`, `screen_main.png`, `screen_load3.png`, `screen_play4.png`.

## 5. DOS (INT 21h) functions used
02, 06, 09, 0E, 19, 1A, 25, 2A, 2C, 2F, 30, 33, 35, 36, 3B, 3C, 3D, 3E, 3F, 40, 41, 42, 43, 44(00,01,07,08), 47, 48, 49, 4A,
4B (DOS shell; can fail), 4C, 4E, 4F, 56, 57, 62.
- File I/O mapped to host dir (C:\ST3 == sandbox). Case-insensitive 8.3 matching; FINDFIRST with wildcard sets (`*.STM`, `*.S3M`, `*.MOD`, `*.*` dirs).
- Environment: Borland c0 reads env block (+ program path after env). Cmdline: `-s5 -a220 -i7 -c1` etc. (Filename arg did not auto-load.)
- Other INTs: 2F (AX=4300 XMS probe, 1600 Windows probe), 33 (mouse; absent OK), 1A, 15, 67 (EMS).

## 6. Timer / PIC
- PIT ch0 reprogrammed: divisor ≈ 16900–17005 (~70.2–70.6 Hz) during sound init; restored to 0 (18.2 Hz) when testing.
- PIC master: mask read/written (port 21h), EOI 20h. IRQ0 (timer) + IRQ7 (SB). Need IRR/ISR priority model.
- Port 61h touched once (speaker/PIT gate).

## 7. Sound
### Sound Blaster Pro (target: -s5 -a220 -i7 -c1)
- Detection: reset port 2x6 (write 1/0), read 2xA expecting 0xAA, status 2xE, for x=1..6. DSP version E1 → we answer 3.02 (SB Pro).
- IRQ test: TC 0x83 (8 kHz) + DSP 14h len 5 (single-cycle) → IRQ7 must fire.
- Playback: D1 (speaker on), TC 0xE9 (~43478 bytes/s), 48h E8 FD (block 65000), **91h high-speed single-cycle** (re-issued from IRQ),
  DMA ch1 mode 0x59 (**auto-init**, single, read), **1 KB ring buffer** at page 5. ST3 **reads DMA current address/count (ports 02/03)**
  to find the play position and refills the ring from the 70 Hz tick.
- Mixer (224/225): reg 0E bit1 = stereo (set for stereo songs → 21739 Hz stereo frames), bit5 filter off; regs 00–1C written (volumes).
- Output format: 8-bit unsigned PCM. Verified audio: `notes/ARMANI_emulated_sbpro.wav`.
### Also probed (not needed for port)
- Gravis Ultrasound at 2x3..? (ports 313/323/.../363 in/out) — absent.
- AdLib/OPL at 388/389 (status reads, timer detect) — absent is fine; **OPL2 emulation needed only for AdLib FM songs** (later).

## 8. Implications for the C emulator (Route A) — design inputs
1. CPU: 386-real-mode interpreter (16-bit default, 0x66/0x67 prefixes, FS/GS, 0F map subset). Cycle-agnostic; virtual time from insn count.
2. HLE BIOS/DOS exactly as in survey.py (trap at F000:nn stubs), with real MCB chain, host FS sandbox.
3. VGA text: 80x50, attr ctl, DAC, plane-2 font capture, 512-char mode, 3DA retrace at 70 Hz.
4. PIT + PIC (IRR/ISR, EOI), virtual clock locked to wall clock (for real-time audio).
5. SB Pro: DSP subset (reset, E1, 40, 48, 14, 1C, 91, 90, D0, D1, D3, E0, E3, F2), 8237 DMA ch1 with readable current addr/count,
   mixer reg 0E stereo; DAC stream → host audio (ALSA/PipeWire) at the programmed rate.
6. Keyboard: terminal input → BIOS INT16 queue with scancodes (incl. F-keys, Alt/Ctrl combos, shift flags at 0040:0017 & AH=02).
7. Renderer: terminal. Options: (R1) Unicode text + truecolor ANSI with glyph mapping for ST3's custom chars; (R2) pixel-exact
   (sixel/kitty) — decide based on the user's terminal.
