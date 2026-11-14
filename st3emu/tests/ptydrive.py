#!/usr/bin/env python3
"""Run inside a real terminal: start st3 in a pty that mirrors this terminal (size, input, output) and type a key
timeline into it, so the real terminal shows st3 being driven.  Writes $WINDOWID to WINFILE (for screenshots).
Usage: python3 -I tests/ptydrive.py WINFILE SECS "t:keys;..." -- st3 args..."""
import os, sys, pty, time, select, fcntl, termios, struct, codecs, tty
winf, secs = sys.argv[1], float(sys.argv[2])
tl = [(float(t), codecs.decode(k, 'unicode_escape').encode('latin-1'))
      for t, k in (x.split(':', 1) for x in filter(None, sys.argv[3].split(';')))]
cmd = sys.argv[sys.argv.index('--') + 1:]
open(winf, 'w').write(os.environ.get('WINDOWID', ''))
ws = fcntl.ioctl(1, termios.TIOCGWINSZ, b'\0' * 8)
pid, fd = pty.fork()
if pid == 0: os.execv(cmd[0], cmd)
fcntl.ioctl(fd, termios.TIOCSWINSZ, ws); os.kill(pid, 28)
old = termios.tcgetattr(0); tty.setraw(0)
t0 = time.time(); i = 0
try:
    while time.time() - t0 < secs:
        el = time.time() - t0
        while i < len(tl) and el >= tl[i][0]: os.write(fd, tl[i][1]); i += 1
        r, _, _ = select.select([0, fd], [], [], 0.01)
        if fd in r:
            try: d = os.read(fd, 1 << 16)
            except OSError: break
            if not d: break
            os.write(1, d)
        if 0 in r: os.write(fd, os.read(0, 4096))   # terminal replies (DA1, cell size) and real keys
finally:
    try: os.write(fd, b'\x1d')
    except OSError: pass
    time.sleep(0.3); termios.tcsetattr(0, termios.TCSADRAIN, old)
