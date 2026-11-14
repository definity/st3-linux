#!/usr/bin/env python3
"""Drive the real st3 binary in a pseudo-terminal, feed keys on a timeline, capture the terminal screen with pyte.
Usage: PYTHONPATH=../tools/pylib python3 tests/ptytest.py OUTPREFIX "t:keys;t:keys;..." [total_secs] [-- st3 args]
  keys are raw strings with escapes like \\x1b[15~ (F5); times in seconds from start.
Writes OUTPREFIX.txt (screen text) and OUTPREFIX.png (colors+chars rendered with a crude font: block shapes only)."""
import os, sys, pty, time, select, struct, fcntl, termios, codecs
import pyte

out = sys.argv[1]
timeline = []
for item in filter(None, sys.argv[2].split(';')):
    t, k = item.split(':', 1)
    timeline.append((float(t), codecs.decode(k, 'unicode_escape').encode('latin-1')))
total = float(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] != '--' else 10.0
extra = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
ROWS, COLS = int(os.environ.get('PTY_ROWS', 52)), int(os.environ.get('PTY_COLS', 82))
pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm-256color'
    os.execv(extra[0], extra)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', ROWS, COLS, 0, 0))
os.kill(pid, 28)  # SIGWINCH
screen = pyte.Screen(COLS, ROWS); stream = pyte.ByteStream(screen)
t0 = time.time(); ti = 0; nbytes = 0
while time.time() - t0 < total:
    el = time.time() - t0
    while ti < len(timeline) and el >= timeline[ti][0]:
        os.write(fd, timeline[ti][1]); ti += 1
    r, _, _ = select.select([fd], [], [], 0.02)
    if r:
        try: data = os.read(fd, 65536)
        except OSError: break
        if not data: break
        nbytes += len(data); stream.feed(data)
try:
    os.write(fd, b'\x1d'); time.sleep(0.5)
except OSError: pass
try: os.kill(pid, 15)
except ProcessLookupError: pass
lines = [''.join(screen.buffer[y][x].data for x in range(COLS)) for y in range(ROWS)]
open(out + '.txt', 'w').write('\n'.join(lines))
print(f'bytes received: {nbytes}')
# colored PNG: each cell 8x8, glyph drawn coarsely (block elements exact, others as 'ink' box)
import zlib
def rgb(c, default):
    if c in ('default', None): return default
    if isinstance(c, str) and len(c) == 6: return tuple(int(c[i:i+2], 16) for i in (0, 2, 4))
    return default
QUAD = {' ': 0, '▘': 1, '▝': 2, '▀': 3, '▖': 4, '▌': 5, '▞': 6, '▛': 7, '▗': 8, '▚': 9, '▐': 10, '▜': 11, '▄': 12, '▙': 13, '▟': 14, '█': 15}
def glyph(ch):
    g = [[0]*8 for _ in range(8)]
    if ch in QUAD:
        b = QUAD[ch]
        for y in range(8):
            for x in range(8):
                q = (2 if y >= 4 else 0) + (1 if x >= 4 else 0)
                g[y][x] = (b >> q) & 1
    elif ch == '▔': g[0] = [1]*8
    elif ch == '▂': g[6] = [1]*8; g[7] = [1]*8
    elif ch == '▕': [g[y].__setitem__(7, 1) for y in range(8)]
    elif ch == '▎': [g[y].__setitem__(x, 1) for y in range(8) for x in (0, 1)]
    elif ch.strip():
        for y in range(1, 7):
            for x in range(1, 6): g[y][x] = 1 if (x + y) % 2 == 0 else 0
    return g
W, H = COLS * 8, ROWS * 8
pix = bytearray(W * H * 3)
for y in range(ROWS):
    for x in range(COLS):
        c = screen.buffer[y][x]
        fg = rgb(c.fg, (200, 200, 200)); bg = rgb(c.bg, (0, 0, 0))
        g = glyph(c.data)
        for yy in range(8):
            for xx in range(8):
                col = fg if g[yy][xx] else bg
                o = ((y*8+yy)*W + x*8+xx)*3; pix[o:o+3] = bytes(col)
raw = b''.join(b'\0' + bytes(pix[y*W*3:(y+1)*W*3]) for y in range(H))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
open(out + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
                                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
