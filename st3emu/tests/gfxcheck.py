#!/usr/bin/env python3
"""Run st3 --gfx MODE in a pty, capture the raw terminal stream, decode the LAST picture it sent and write it as PPM.
Usage: python3 -I tests/gfxcheck.py MODE OUT.ppm SECS "t:keys;..." -- st3 args...   (pty is 100x60, 8x16 px cells)
Decodes: iterm (OSC 1337 PNG), kitty (APC G chunks, PNG), sixel (DCS q).  Prints stream stats.
GFXCHECK_ANSWER='\x1b[?62;4c' (the DA1 reply) makes the pty report 0 pixels and answer st3's queries."""
import os, sys, pty, time, select, struct, fcntl, termios, codecs, base64, zlib, re

mode, outp, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
timeline = []
for item in filter(None, sys.argv[4].split(';')):
    t, k = item.split(':', 1)
    timeline.append((float(t), codecs.decode(k, 'unicode_escape').encode('latin-1')))
cmd = sys.argv[sys.argv.index('--') + 1:]
ROWS, COLS = 60, 100
CW, CH = map(int, os.environ.get('GFXCHECK_CELL', '8x16').split('x'))   # cell size in pixels
pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm-256color'; os.execv(cmd[0], cmd)
ANSWER = os.environ.get('GFXCHECK_ANSWER')   # set: report 0 pixels and answer the cell-size / DA1 queries instead (like hterm)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', ROWS, COLS, 0 if ANSWER else COLS * CW, 0 if ANSWER else ROWS * CH))
os.kill(pid, 28)
buf = bytearray(); t0 = time.time(); ti = 0
while time.time() - t0 < secs:
    el = time.time() - t0
    while ti < len(timeline) and el >= timeline[ti][0]: os.write(fd, timeline[ti][1]); ti += 1
    r, _, _ = select.select([fd], [], [], 0.02)
    if r:
        try: d = os.read(fd, 1 << 20)
        except OSError: break
        if not d: break
        buf += d
        if ANSWER and b'\x1b[16t' in d: os.write(fd, b'\x1b[6;%d;%dt' % (CH, CW))
        if ANSWER and b'\x1b[c' in d: os.write(fd, ANSWER.encode())
try: os.write(fd, b'\x1d'); time.sleep(0.3)
except OSError: pass
try: os.kill(pid, 15)
except ProcessLookupError: pass
print(f'stream bytes {len(buf)} ({len(buf)/secs/1024:.0f} KiB/s average)')

def png_decode(p):
    i = 8; idat = b''; pal = None
    while i < len(p):
        n = struct.unpack('>I', p[i:i+4])[0]; t = p[i+4:i+8]; c = p[i+8:i+8+n]; i += 12 + n
        if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10]); assert bd == 8 and ct == 3
        elif t == b'PLTE': pal = c
        elif t == b'IDAT': idat += c
    raw = zlib.decompress(idat); out = bytearray(); prev = bytearray(w)
    for y in range(h):
        f = raw[y*(w+1)]; line = bytearray(raw[y*(w+1)+1:(y+1)*(w+1)])
        if f == 2: line = bytearray((line[x] + prev[x]) & 255 for x in range(w))
        else: assert f == 0
        out += line; prev = line
    rgb = bytearray()
    for v in out: rgb += pal[v*3:v*3+3]
    return w, h, rgb

if mode == 'iterm':
    imgs = re.findall(rb'\x1b\]1337;File=([^:]*):([A-Za-z0-9+/=]*)\x07', bytes(buf))
    print('images', len(imgs), 'last args', imgs[-1][0].decode() if imgs else None)
    w, h, rgb = png_decode(base64.b64decode(imgs[-1][1]))
elif mode == 'kitty':
    chunks = re.findall(rb'\x1b_G([^;]*);([A-Za-z0-9+/=]*)\x1b\\', bytes(buf))
    imgs = []; cur = b''
    for ctl, data in chunks:
        cur += data
        if b'm=1' not in ctl: imgs.append(cur); cur = b''
    starts = [c for c, _ in chunks if c.startswith(b'a=T')]
    print('images', len(imgs), 'last args', starts[-1].decode() if starts else None)
    w, h, rgb = png_decode(base64.b64decode(imgs[-1]))
else:
    # composite every sixel at its cursor position on a ROWS*16 x COLS*8 canvas (partial updates repaint bands)
    W, H = COLS * CW, ROWS * CH; bad6 = 0
    canvas = bytearray(W * H * 3)
    six = re.findall(rb'\x1b\[(\d+);(\d+)H\x1bP0;1;0q(.*?)\x1b\\', bytes(buf), re.S)
    print('images', len(six), 'bytes per image avg', sum(len(x[2]) for x in six) // max(1, len(six)))
    minx = miny = 1 << 30; maxx = maxy = 0
    for prow, pcol, data in six:
        ox, oy = (int(pcol) - 1) * CW, (int(prow) - 1) * CH
        s = data.decode('latin-1')
        m = re.match(r'"1;1;(\d+);(\d+)', s); w, h = int(m.group(1)), int(m.group(2)); s = s[m.end():]
        bad6 += h % 6 != 0
        minx = min(minx, ox); miny = min(miny, oy); maxx = max(maxx, ox + w); maxy = max(maxy, oy + h)
        cols = {}; x = y = 0; c = (0, 0, 0); i = 0
        while i < len(s):
            ch = s[i]
            if ch == '#':
                m = re.match(r'#(\d+)(;2;(\d+);(\d+);(\d+))?', s[i:]); i += m.end()
                if m.group(2): cols[int(m.group(1))] = tuple(round(int(m.group(k)) * 255 / 100) for k in (3, 4, 5))
                c = cols[int(m.group(1))]; continue
            if ch == '$': x = 0; i += 1; continue
            if ch == '-': x = 0; y += 6; i += 1; continue
            rep = 1
            if ch == '!':
                m = re.match(r'!(\d+)', s[i:]); rep = int(m.group(1)); i += m.end(); ch = s[i]
            b = ord(ch) - 63
            if b:
                for k in range(6):
                    if b >> k & 1 and y + k < h:
                        row = (oy + y + k) * W
                        for xx in range(x, min(x + rep, w)): o = (row + ox + xx) * 3; canvas[o:o+3] = bytes(c)
            x += rep
            i += 1
    w, h = maxx - minx, maxy - miny
    print('sixels whose height is not a multiple of 6:', bad6)
    rgb = bytearray()
    for yy in range(miny, maxy): rgb += canvas[(yy * W + minx) * 3:(yy * W + maxx) * 3]
open(outp, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(rgb))
print(f'decoded {w}x{h} -> {outp}')
