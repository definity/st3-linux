#!/usr/bin/env python3
"""Render notes/vga_state.json + notes/font_plane2.bin to a PNG (pixel-exact VGA text mode, 8-px wide cells).
Usage: python3 -I tools/render.py out.png [scale]"""
import json, zlib, struct, sys
st = json.load(open('notes/vga_state.json')); font = open('notes/font_plane2.bin', 'rb').read()
dac, attr, rows, text, ch = st['dac'], st['attr'], st['rows'], st['text'], st['charh'] or 8
scale = int(sys.argv[2]) if len(sys.argv) > 2 else 1
FONTB = st.get('seq3', 0x04) is not None and len(sys.argv) <= 3
blink = attr[0x10] & 0x08
def rgb(ix):   # 4-bit attribute color -> DAC
    p = attr[ix & 15] & 0x3F
    if attr[0x10] & 0x80: p = (p & 0x0F) | ((attr[0x14] & 3) << 4)
    p |= (attr[0x14] & 0x0C) << 4
    return tuple(min(255, dac[p*3+k] * 255 // 63) for k in range(3))
W, H = 80*8, rows*ch
pix = bytearray(W*H*3)
for r in range(rows):
    for c in range(80):
        code, at = text[(r*80+c)*2], text[(r*80+c)*2+1]
        fg = rgb(at & 15); bg = rgb((at >> 4) & (7 if blink else 15))
        for y in range(ch):
            bits = font[(0x4000 if (at & 8) and FONTB else 0) + code*32 + y]
            for x in range(8):
                col = fg if bits & (0x80 >> x) else bg
                o = ((r*ch+y)*W + c*8 + x)*3; pix[o:o+3] = bytes(col)
def png(path, w, h, data, sc):
    raw = bytearray()
    for y in range(h*sc):
        raw.append(0); row = data[(y//sc)*w*3:(y//sc+1)*w*3]
        if sc > 1: row = b''.join(row[i:i+3]*sc for i in range(0, len(row), 3))
        raw += row
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w*sc, h*sc, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
png(sys.argv[1], W, H, pix, scale)
print('wrote', sys.argv[1], W*scale, 'x', H*scale)
