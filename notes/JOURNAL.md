# ST3 port journal (append-only, newest at bottom)
Goal: Route A = single static Linux ELF, console (terminal) UI, embeds ST3.EXE + 8086 emulator + DOS/BIOS/VGA-text/SB shims.
Then Route B (native rewrite) only after A works.
Milestone 1: unpack ST3.EXE (custom protector "FCP/IV"), document it.
Work dir: work/  (originals in ../ untouched)
Scratch: a temporary directory outside the repo

## Log
- [t0] Copied files to work/. Inventory done (see chat). ST3.EXE: MZ, hdr 32B, no relocs, entropy 8.0,
  entry 1e80:0006, SS:SP=1e80:0074, minalloc 0x25e7 paras. Stub = last 1292 bytes (file off 0x1e820).
  Stub: mov sp,74;ret trick, decrypt loop (sub [bx],ax x0x18c words @0x178 key 5f27), int3 handler, smsw check, int13/int10 fingerprint.
- [m1.1] Env: pip --target tools/pylib has unicorn 2.1.4 + capstone 5.0.9 (use PYTHONPATH=tools/pylib). No venv (ensurepip missing).
- [m1.2] tools/trace.py = Unicorn 16-bit tracer w/ minimal DOS/BIOS HLE; writes notes/trace.log (first-exec insns, INTs) + notes/mem_after.bin.
  Findings so far (stub at 1E80, load seg = PSP+0x10):
  * Stub hooks IVT 0..3 (div0, INT1 single-step, NMI, INT3) to its own handlers (swap-in, saves old).
  * Layered decrypt: each layer = (sub|xor) [bx],key loop; keys derived from trap-flag stepping. INT3 handler @00FF sets TF
    (xor [bp+6],0x100) so INT1 handler @00FE runs per instruction and does 'inc ax' => decrypt KEY DEPENDS ON SINGLE-STEP COUNT (anti-trace).
    Unicorn delivers TF/INT1 faithfully, so plain emulation decrypts correctly. Do NOT step-patch the code.
  * Checksum loop over stub bytes 0x91..0x123 (anti-tamper) -> result in DX used as a later key.
  * Final: jumps to far segment 3BE4:00F4 (in LOAD-relative terms) -> likely the real unpacker/relocation or program. Run hit 2M insn cap there.
- [m1.3] BREAKTHROUGH: emulation unpacks cleanly in ~6s / 2.5M insns (STOP21=1 python3 tools/trace.py ST3.EXE 150000000).
  Layers: (1) stub@1E80 checksums+decrypts whole image in place, wipes itself, restores IVT, retf ->
          (2) stub copy@1E36 (and 1E20) re-run, (3) PKLITE-style LZ decompressor @ (load+3BE4) writes real image,
          applies relocs, then 'ljmp cs:[bx]' -> real entry at (load+)2C79:0012 = 'mov ah,30h; int 21h' (C/Pascal startup).
  Anti-debug seen: IVT0-3 hijack, TF single-step key dependence, INT13 ah=8 hd probe, PUSH SP test, SMSW (V86) test, stub self-checksum.
  Next: tools/unpack.py -> run at 2 load segs, diff to recover relocs, write ST3U.EXE (plain MZ). Verify w/ ndisasm + strings.
- [m1.4] tools/unpack.py (2 load segs -> diff -> 4211 relocs) + tools/mkexe.py => work/ST3U.EXE (293,856 B, plain MZ, unpacked).
  Verified: strings Scream/Tracker/Blaster/Gravis/Adlib present; entry 2C79:0012 = Borland C c0 startup (mov ah,30h; DGROUP=3A94 rel).
  Entry CS:IP=2C79:0012, hdr SS:SP=41DE:1000 (startup sets SS=DGROUP itself). No leftover FCP/PKLITE signatures.

## Milestone 2 — survey (2026-11-14, on power; user asked: methodical, step by step)
Plan: 2a static scan of ST3U.EXE (INT n imm, IN/OUT imm ports, strings for hw names) ; 2b dynamic I/O+INT logger (Unicorn) with
progressive HLE until program idles at keyboard ; 2c write notes/HWMAP.md (ports, ints, memory model: EMS/XMS?) ; 2d only then design C emulator.
- [m2a] Static scan: Borland C. Usage text found: -s1 SB, -s2 GUS, -s5 SBPro, -m mixspeed, -a addr, -i irq, -c dma, -b buf, -f colors,
  -l alt dos shell, -d date, -p pause, -x scope. Env strings NOSOUND=1, U2CRYPT=1. EMS optional (opens EMMXXXX0, INT67). No XMS strings.
  Error "Gravis Ultrasound / SoundBlaster / Soundblaster Pro not found." Mouse INT33 present. Created work/sandbox/ (copy of runtime files).
- [m2b] tools/survey.py written: Unicorn + HLE (IVT vectoring to F000:nn stubs trapped by code hook -> Python HLE -> IRET keeping CF/ZF),
  DOS file I/O mapped to sandbox/ (C:\ST3 == sandbox), FINDFIRST, mem alloc, INT10 text, INT16 scripted keys, PIT IRQ0 injection,
  port logging. Output notes/survey_<tag>.txt + .log.
  RUN nosnd: boots to "Sound initialization failed... Press any key" (~1M insns). Probes: SB reset 2x6/2xA/2xE for x=1..6,
  GUS 3x3 for x=1..6, OPL 388/389 (detect + delay loop). VGA regs 3C0/3C2/3C4/3C8/3C9/3CE programmed (palette+font upload to plane 2).
  -> emulator needs VGA plane/font-load handling; terminal renderer needs custom glyph mapping. NOSOUND=1 env didn't skip autodetect.
  Hooked vectors so far: INT00, INT23, INT24 (no INT08/09 yet).
- [m2c] RUN ui1 (--keys ' '): passes "press any key", "Output: CARD:<NO SOUNDCARD DETECTED> MIX@:44Khz", reaches MAIN UI:
  main loop polls INT16 AH=01 (14.9k polls). Start screen logo is drawn with CUSTOM FONT glyph tiles (chars 0x01..0xFF re-defined).
  => Renderer decision pending: (a) Unicode approximation of glyphs (sextant/braille), (b) pixel-exact via sixel/kitty/iTerm2 image
  protocol in terminal. Need to check what the user's terminal (Chromebook) supports. Keyboard = BIOS INT16 (no INT9 hook w/o sound).
- [m2d] VGA model validated: tools/survey.py captures DAC (3C8/3C9), attr ctl (3C0 flip-flop reset by 3DA read), font plane 2
  (upload seq: GC6=0C,GC5=00,SEQ4=06,GC4=02,SEQ2=04 -> font at B800 window; restore SEQ2=03,GC6=0E,GC5=10,SEQ4=03,GC4=00).
  tools/render.py -> notes/screen_title.png = AUTHENTIC ST3 title screen (cursive "Scream Tracker" logo).
  KEY FACT: ST3 uses 512-char mode: attr fg bit3 selects font block @plane2+0x4000 (logo tiles); block0 = ST3 UI font w/ custom bevel glyphs.
  Font uploaded only 3x at startup (not per-frame) -> glyph set is static (good for terminal glyph mapping).
  Console renderer consequence: pure-text terminal can't show logo tiles exactly -> plan pluggable renderer:
    R1 Unicode text (CP437->UTF8 + hand map for ST3 custom glyphs; logo approximated), R2 pixel-exact via sixel/kitty in terminal.
  Insn counter in survey.py is approximate (stub traps end chunk early but count CHUNK) — virtual time inflated; irrelevant for survey.
- [m2e] Key scripting added (--keys ' {W}x{W}', tokens {ESC}{F1}..{ALT-x}{CTRL-l}{W300}). Rendered notes/screen_main.png:
  REAL ST3 PATTERN EDITOR works ('x' -> D-3 note). FreeMem 254K, FreeEMS 0K. Next: SB emulation in survey for sound path.
- [m2f] SB Pro model in survey.py (DSP, 8237 DMA, PIC IRR/ISR). With "-s5 -a220 -i7 -c1": "Output: CARD:SoundBlaster Pro IO:220 IRQ:7 DMA:1 MIX@:44Khz".
  Sound protocol: IRQ test = TC 0x83 + DSP14 len 5; playback = D1, TC 0xE9 (~43.5kHz), 48 E8 FD (blk 65000), 91 (HS single-cycle,
  re-issued), DMA ch1 mode 0x59 auto-init, 1KB ring @ page 5. Mixer 0E=20 (mono, filter off). ST3 polls DMA pos (in 02/03).
  PIT reprogrammed ~70Hz (divisor measured from VGA retrace polling!) -> emulator must model 3DA retrace at 70Hz in virtual time.
  Hooks: INT08 -> 24ED:28CD (player tick), INT0F -> 24ED:635F (SB IRQ7), INT00, INT23, INT24.
- [m2g] Load dialog (CTRL-L) works; needs time between keys (dir scan). Cmdline filename did NOT auto-load. Copied ARMANI.S3M, DEF.S3M,
  TECHNOLO.MOD into sandbox root. keys " {W}{CTRL-l}{W2000}{DOWN}{W2000}{ENTER}" => OPEN ARMANI.S3M. Next: F5 play + WAV capture.
- [m2h] Load works (needs 'y' at "Current song is about to be erased!"). Song "Armani Showers" loaded. F5 => ST3 self-check:
  "!! CRASH WARNING ... Pattern 0(1E64:5BA8) memory allocation error detected." WAV silent. Suspect: ST3 validates DOS MCB chain
  (survey allocator has no MCBs). Investigating the check (find string xref).
- [m2i] RE: check at linear 0x1B4F4..: for pattern i in 0..99: seg=patseg[i]; byte at (seg-1):0 must == 'M' (0x4D) => DOS MCB check.
  (Same loop for instruments 0..0x62 at 0x1B48C.) => DOS layer MUST maintain a real MCB chain in memory. Implementing in survey.py.
- [m2j] MCB chain implemented in survey.py (FIRST_MCB=0x6FF env, PSP-1 program 'Z'). CRASH WARNING gone.
  *** ST3 PLAYS ARMANI.S3M *** "Playing; loop:0 ord:003/010 pat:03 row:21". notes/screen_play4.png, audio notes/ARMANI_emulated_sbpro.wav
  (8-bit unsigned stereo 21739 Hz; peaks 329/438 Hz = E4/A4; no clipping; L/R uncorrelated). Sound path fully proven.
  Next: CPU opcode census during playback (8086/286/386?), then notes/HWMAP.md.
- [m2k] Static sweep: 386 code present (shl ecx,10h @0x27B68, push/pop fs, 0F 8x jcc) clustered 0x26000-0x2B000 = sound/mixer seg (24ED).
  Rest = 186/286 (enter/leave/push imm/imul imm/shift imm). => CPU core must do 386 REAL MODE (66/67 prefixes, FS/GS, 0F ops). Census running.
- [m2l] Opcode census done (notes/survey_census.txt). 386 o32 ops in mixer (24ED): add/mov/shl/shr/push/pop/or/xor/movzx/sub/div/cmp.
  No FPU, no PM. HWMAP.md §1a updated. MILESTONE 2 SURVEY COMPLETE -> notes/HWMAP.md is the emulator spec.
  Open questions for user: renderer (Unicode text vs sixel/kitty pixel) ; confirm own C CPU core + Unicorn differential testing.

## Milestone 3 — C emulator (started 2026-11-14 ~00:30)
- [m3a] notes/ARCH.md (module layout). st3emu/src/cpu.{h,c}: 386 real-mode interpreter (~700 lines). Makefile builds libst3cpu.so
  (with tests/cpu_api.c) for ctypes. tests/fuzz_cpu.py = differential fuzzer vs Unicorn (random 1 instruction, compare regs/segs/ip/
  flags-minus-undefined/memory 0..0x80000).
  Harness lessons: MUST uc.ctl_remove_cache() per case (QEMU TB cache served stale code); REP needs run-to-next-ip (count=1 = 1 iteration).
  Bugs fixed so far: ENTER w/ 66 on 16-bit stack must write BP only; C6/C7 reg!=0 must be #UD.
  Accepted divergences: UC rejects 66/67 forms producing offsets >64K (#GP in real CPU too; ST3 never does it).
- [m3b] Fuzz 20k: 19100 ok. Remaining = UC-side quirks: #DE register writes lost in Unicorn exception hook (verified standalone),
  C7/7 mod3 = XBEGIN in QEMU, F7 /1 alias, 66/67 >64K offsets (#GP). Fuzzer now skips #DE compare. Next: multi-seed 100k run.
  io.c written (PIC/PIT w/ readable latch, DMA w/ readable pos, SB Pro DSP + pcm_sink). machine.h written.
- [m3c] FULL C EMULATOR WORKS HEADLESS: src/{machine,io,vga,bios,dos,keys}.c + tests/headless.c -> st3emu/st3head.
  Title screen pixel-identical (out/c_title.png). SB Pro AUTODETECTED (no switches). Load+play ARMANI: "Playing ord:002/010",
  out/c_play.wav stereo 21739Hz musical. Host speed: 225 MIPS idle-ish, 102 MIPS during playback (virtual target 25 MIPS) -> 4x headroom.
  Usage: st3head ROOT EXE SECS "KEYSCRIPT" OUTPREFIX  (waits {Wn} in virtual ms).
  Next: terminal front-end (raw tty, ANSI truecolor, glyph mapping, key decode) + pw-play audio + realtime throttle -> st3 ELF.
- [m3d] Remaining fuzz div/idiv mismatches investigated: isolated repro (divisors 0100/31A4/31A5/FFFF) -> C and Unicorn IDENTICAL
  (fault <=31A4, same quotient/remainder otherwise). Fuzz noise = Unicorn exception-path harness quirk. CPU core considered verified
  (~95.5% exact match on random ops; all residuals classified: UC #GP on >64K offsets, XBEGIN, F7/1 alias, #DE hook quirk).
- [m3e] Front-end written: src/render.c (CP437->Unicode + ST3 hand-map for bevel glyphs E0-EF/F4/F9/FB/FC/FD, auto quadrant map
  for font block 1 / unknown glyphs, truecolor diff renderer, SEQ3 char-map select), src/audio.c (resample->48k s16 -> pw-play pipe,
  non-blocking), src/main.c (raw tty, alt screen, CSI/SS3 key decode, wall-clock lock, Ctrl-] quit), tools_embed.py embeds ../ST3U.EXE.
  `make st3` => st3emu/st3 static ELF 1.75MB. VGA font capture now honors GC6 window (A000/B000/B800).
  Glyph census over F1-F10/SF9/ESC screens: block0 custom codes E0-E4,E6-EF,F4,F9,FB,FC,FD only.
- [m3f] ROUTE A DELIVERED: work/dist/ = st3 (static ELF, stripped 1.2MB) + ST3.SET + ########.ST3 + i/ + s/ + docs/ + README.txt.
  Verified via pty+pyte (tests/ptytest.py): title (logo in quadrant blocks), load ARMANI via Ctrl-L, play F5, Ctrl-Q+Y clean exit,
  small-terminal notice, Alt-G dir change (Alt needs shift flags: front-end sets them; headless driver doesn't).
  Audio: captured resampled stream = music; realtime lock exact (vtime==wall over 25s), 0 dropped bytes; pw-play accepts stream.
  Fix: font block1 glyphs identical to block0 map like block0 (bright text uses block1 after 4th font upload).
  Not yet heard by a human. Open: sixel pixel mode (optional), EMS (optional), OPL2 FM (AdLib songs), Route B.
- [m4a] 2026-11-14 USER TEST of Route A: "sound isn't smooth, bad reception staccato" + "tracker not viewable while playing".
  Repo put under git (work/, .gitignore excludes pylib, build products, out/, sandbox/, *.wav).
  AUDIO root causes (both in audio.c, never exercised by headless tests which bypass it):
   1. flush_out wrote on_n BYTES but on_n counts int16 SAMPLES -> only first half of every buffer reached pw-play.
   2. sb_update hands audio_feed 1-2 bytes per call; frames=n/2 dropped odd bytes in stereo (17% loss + L/R swaps).
      Fix: carry partial frame (a->part).
   3. No lead: wall-locked production left ~0 queued -> 1-5% period underruns. Fix: while audio active, pace on
      pipe fill (FIONREAD) to 70 ms, capped wall+0.15 / floored wall-0.2 (ST3_AUDIO_MS env overrides).
  Tool: tests/audiosink.py = fake 48k device (fixed quanta, real-time), counts underruns. Before: 99.9% underruns;
  after: 0 underruns (1024 and 2048 quanta), output 192000 B/s exact; real pw-play queue 47-70 ms, 0 drops. User heard it: "sounds awesome".
  DISPLAY: not a bug. F5 Info Screen bottom window defaults to "None"; PgUp/PgDn selects Track-5/8/18 (manual 3.5).
  The user's "magic key" was likely PgDn. VU bars verified moving (out/d_info.png). CRTC start address never written (no page flip).
  Side project requested: WebAssembly build running in Chrome (after Route A is solid).
