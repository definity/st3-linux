#!/usr/bin/env python3
"""Fake audio device for st3 --audio: consumes s16 stereo 48 kHz from stdin in fixed quanta on a real-time clock
(like PipeWire does) and reports underruns.  Usage: st3 --audio "python3 -I tests/audiosink.py REPORT [quantum_frames]"."""
import os, sys, time, select, signal
rep = sys.argv[1]; Q = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
need = Q * 4; period = Q / 48000.0
signal.signal(signal.SIGTERM, lambda *a: (_ for _ in ()).throw(KeyboardInterrupt))
buf = bytearray(); fd = 0; os.set_blocking(fd, False)
started = False; t_next = None; quanta = under = 0; short_bytes = 0; fills = []; eof = False
empty_run = 0
def report():
    fs = sorted(fills)
    with open(rep + '.tmp', 'w') as f:
        f.write(f'quanta {quanta} underruns {under} ({100.0*under/max(1,quanta):.1f}%) missing_ms {short_bytes/4/48:.0f} '
                f'fill_ms p10 {fs[len(fs)//10]/192 if fs else 0:.1f} median {fs[len(fs)//2]/192 if fs else 0:.1f}\n')
    os.replace(rep + '.tmp', rep)
last_rep = 0
try:
  while not eof:
      if time.monotonic() - last_rep > 1: report(); last_rep = time.monotonic()
      timeout = 0.002 if t_next is None else max(0.0, t_next - time.monotonic())
      r, _, _ = select.select([fd], [], [], timeout)
      if r:
          try:
              d = os.read(fd, 1 << 16)
              if not d: eof = True
              buf += d
          except BlockingIOError: pass
      now = time.monotonic()
      if not started and len(buf) >= need:   # device starts on first full quantum
          started = True; t_next = now
      if started and now >= t_next:
          fills.append(len(buf))
          quanta += 1
          if len(buf) < need: under += 1; short_bytes += need - len(buf)
          empty_run = empty_run + 1 if not buf else 0
          del buf[:need]
          t_next += period
          if empty_run > 25:   # stream stopped (>0.5 s nothing): like a device going idle; stats restart with the next stream
              started = False; t_next = None; quanta = under = short_bytes = 0; fills = []; empty_run = 0
except KeyboardInterrupt: pass
report()
