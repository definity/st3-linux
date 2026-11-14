#!/usr/bin/env python3
"""Write FMTEST.S3M: a small original AdLib (OPL2 FM) test song for ST3, built from TECH.DOC's S3M layout.
Usage: python3 -I tools/mkfmsong.py OUT.S3M
4 melodic FM patches (bass, two pads, bell lead) on AdLib melody channels A1-A4, plus bass drum, snare
and hi-hat on the AdLib rhythm channels.  Chords Am-F-C-G, 2 patterns, order 0 1 0 1, speed 6 tempo 125."""
import struct, sys

def note(name, octave):
    return (octave << 4) | ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'].index(name)

# instrument: (type, name, D00..D0A raw OPL register bytes [mod20,car20,mod40,car40,mod60,car60,mod80,car80,modE0,carE0,C0], volume)
INS = [
    (2, 'FM Bass',      [0x21, 0x21, 0x1A, 0x00, 0xF4, 0xF3, 0x46, 0x56, 0x00, 0x00, 0x0A], 63),
    (2, 'FM Organ Pad', [0x22, 0x21, 0x14, 0x06, 0x83, 0x72, 0x14, 0x15, 0x01, 0x00, 0x01], 48),
    (2, 'FM Bell Lead', [0x05, 0x01, 0x1C, 0x00, 0xF2, 0xF2, 0x53, 0x54, 0x00, 0x01, 0x06], 56),
    (3, 'FM Bass Drum', [0x01, 0x01, 0x0A, 0x00, 0xF8, 0xF6, 0x77, 0x68, 0x00, 0x00, 0x08], 63),
    (4, 'FM Snare',     [0x01, 0x01, 0x00, 0x00, 0xF7, 0xF7, 0x37, 0x37, 0x00, 0x00, 0x00], 63),
    (7, 'FM Hihat',     [0x01, 0x01, 0x00, 0x00, 0xF9, 0xF9, 0x4A, 0x4A, 0x00, 0x00, 0x00], 50),
]
CHAN_TYPES = [16, 17, 18, 19, 25, 26, 29]   # A1 bass, A2 pad, A3 pad, A4 lead, BD, SD, HH
CHORDS = [('A', 'C', 'E', 'A'), ('F', 'A', 'C', 'F'), ('C', 'E', 'G', 'C'), ('G', 'B', 'D', 'G')]
LEAD = [  # per chord, 8 notes on even rows (name, octave)
    [[('E', 5), ('A', 5), ('C', 6), ('B', 5), ('A', 5), ('E', 5), ('C', 5), ('E', 5)],
     [('F', 5), ('A', 5), ('C', 6), ('A', 5), ('F', 5), ('C', 5), ('A', 4), ('C', 5)],
     [('G', 5), ('E', 5), ('C', 5), ('E', 5), ('G', 5), ('C', 6), ('E', 6), ('C', 6)],
     [('B', 5), ('G', 5), ('D', 5), ('G', 5), ('B', 5), ('D', 6), ('B', 5), ('G', 5)]],
    [[('A', 5), None, ('C', 6), None, ('E', 6), ('D', 6), ('C', 6), ('B', 5)],
     [('A', 5), None, ('F', 5), None, ('A', 5), ('G', 5), ('F', 5), ('E', 5)],
     [('E', 5), None, ('G', 5), None, ('C', 6), ('B', 5), ('C', 6), ('D', 6)],
     [('D', 6), None, ('B', 5), None, ('G', 5), ('A', 5), ('B', 5), None]],
]

def pattern(variant):
    rows = [[] for _ in range(64)]   # (channel, note, ins, vol or None)
    for c, ch in enumerate(CHORDS):
        base = c * 16
        rows[base].append((1, note(ch[0], 4), 2, None)); rows[base].append((2, note(ch[1], 4) if ch[1] != 'C' else note('C', 5), 2, None))
        for r in range(0, 16, 4):
            rows[base + r].append((0, note(ch[0], 2 if r % 8 == 0 else 3), 1, None))
        for i, n in enumerate(LEAD[variant][c]):
            if n: rows[base + i * 2].append((3, note(*n), 3, None))
        for r in (0, 8): rows[base + r].append((4, note('C', 4), 4, None))
        for r in (4, 12): rows[base + r].append((5, note('C', 4), 5, None))
        for r in range(2, 16, 4): rows[base + r].append((6, note('C', 5), 6, None))
        if variant == 1: rows[base + 14].append((5, note('C', 4), 5, 40))
    data = bytearray()
    for evs in rows:
        for (chn, n, ins, vol) in sorted(evs):
            data.append(chn | 0x20 | (0x40 if vol is not None else 0)); data += bytes([n, ins])
            if vol is not None: data.append(vol)
        data.append(0)
    return struct.pack('<H', len(data) + 2) + data

def instrument(t, name, d, vol):
    b = bytearray(0x50)
    b[0] = t
    fn = name.replace(' ', '')[:8].upper().encode() + b'.INS'
    b[1:1 + len(fn)] = fn[:12]
    b[0x10:0x1B] = bytes(d); b[0x1C] = vol
    b[0x20:0x24] = struct.pack('<I', 8363)
    b[0x30:0x30 + len(name)] = name.encode()
    b[0x4C:0x50] = b'SCRI'
    return b

orders = [0, 1, 0, 1]
pats = [pattern(0), pattern(1)]
hdr = bytearray(0x60)
hdr[0:len(b'FM Test (OPL2)')] = b'FM Test (OPL2)'
hdr[0x1C] = 0x1A; hdr[0x1D] = 16
struct.pack_into('<HHHHHH', hdr, 0x20, len(orders), len(INS), len(pats), 0, 0x1320, 2)
hdr[0x2C:0x30] = b'SCRM'
hdr[0x30] = 64; hdr[0x31] = 6; hdr[0x32] = 125; hdr[0x33] = 0xB0; hdr[0x34] = 0; hdr[0x35] = 0
for i in range(32): hdr[0x40 + i] = CHAN_TYPES[i] if i < len(CHAN_TYPES) else 255
out = bytearray(hdr) + bytes(orders)
ins_ptr_at = len(out); out += bytes(2 * len(INS))
pat_ptr_at = len(out); out += bytes(2 * len(pats))
def align():
    while len(out) % 16: out.append(0)
for i, ins in enumerate(INS):
    align(); struct.pack_into('<H', out, ins_ptr_at + 2 * i, len(out) // 16); out += instrument(*ins)
for i, p in enumerate(pats):
    align(); struct.pack_into('<H', out, pat_ptr_at + 2 * i, len(out) // 16); out += p
open(sys.argv[1], 'wb').write(out)
print(f'wrote {sys.argv[1]}: {len(out)} bytes, {len(INS)} FM instruments, {len(pats)} patterns')
