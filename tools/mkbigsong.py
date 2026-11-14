#!/usr/bin/env python3
"""Write BIGTEST.S3M: an original test song whose samples need far more memory than DOS has free, so ST3 must keep
them in EMS.  Usage: python3 -I tools/mkbigsong.py OUT.S3M [n_samples] [sample_bytes]
Sample k (k = 0..n-1) is a looped 8-bit sine whose pitch is 220 Hz * 2^(k/12) when played at C-4 (8363 Hz C4SPD
ratio: the sample holds 8363/f samples per period), with a short fade-in so each note is identifiable.
Pattern: sample k plays C-4 on channel 1 at row 4k (speed 6, tempo 125 -> 0.48 s per note); n notes, then stop."""
import math, struct, sys
out = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 16
SZ = int(sys.argv[3]) if len(sys.argv) > 3 else 64000
def sample(k):
    f = 220.0 * 2 ** (k / 12)
    period = 8363.0 / f
    n = int(SZ // period * period) if SZ > period else SZ   # whole periods so the loop is seamless
    return bytes(int(128 + 100 * math.sin(2 * math.pi * i / period)) & 255 for i in range(n)), n
hdr = bytearray(0x60)
hdr[0:12] = b'EMS bigtest\0'
hdr[0x1C] = 0x1A; hdr[0x1D] = 16
rows_needed = 4 * N + 4
npat = (rows_needed + 63) // 64
struct.pack_into('<HHHHHH', hdr, 0x20, npat + 1, N, npat, 0, 0x1320, 2)   # flags 0, cwt 1320, ffi 2 = unsigned
hdr[0x2C:0x30] = b'SCRM'
hdr[0x30] = 64; hdr[0x31] = 6; hdr[0x32] = 125; hdr[0x33] = 0xB0
for i in range(32): hdr[0x40 + i] = 0 if i == 0 else 255
orders = list(range(npat)) + [255]
out_b = bytearray(hdr) + bytes(orders)
ins_at = len(out_b); out_b += bytes(2 * N)
pat_at = len(out_b); out_b += bytes(2 * npat)
def align():
    while len(out_b) % 16: out_b.append(0)
samples = [sample(k) for k in range(N)]
ins_blocks = []
for k in range(N):
    align(); struct.pack_into('<H', out_b, ins_at + 2 * k, len(out_b) // 16)
    ins_blocks.append(len(out_b)); out_b += bytes(0x50)
for p in range(npat):
    rows = []
    for r in range(64):
        g = p * 64 + r
        d = bytearray()
        if g % 4 == 0 and g // 4 < N: d += bytes([0x20, 0x40, g // 4 + 1])        # ch0: C-4, instrument k+1
        if g == 4 * N: d += bytes([0x80, 0x03, 0x00])                             # Cxx would continue; C00 at end
        rows.append(bytes(d) + b'\0')
    data = b''.join(rows)
    align(); struct.pack_into('<H', out_b, pat_at + 2 * p, len(out_b) // 16)
    out_b += struct.pack('<H', len(data) + 2) + data
for k, (data, n) in enumerate(samples):
    align(); para = len(out_b) // 16
    b = bytearray(0x50); b[0] = 1
    fn = b'TONE%02d.SMP' % k; b[1:1 + len(fn)] = fn
    b[0x0D] = (para >> 16) & 255; struct.pack_into('<H', b, 0x0E, para & 0xFFFF)
    struct.pack_into('<III', b, 0x10, n, 0, n)
    b[0x1C] = 64; b[0x1F] = 1                                                     # volume 64, flags: loop
    struct.pack_into('<I', b, 0x20, 8363)
    nm = b'Tone %02d (%.1f Hz)' % (k, 220.0 * 2 ** (k / 12)); b[0x30:0x30 + len(nm)] = nm
    b[0x4C:0x50] = b'SCRS'
    out_b[ins_blocks[k]:ins_blocks[k] + 0x50] = b
    out_b += data
open(out, 'wb').write(out_b)
print(f'wrote {out}: {len(out_b)} bytes, {N} samples x ~{SZ} bytes = {sum(n for _, n in samples)} sample bytes')
