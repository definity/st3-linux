#!/usr/bin/env python3
"""Write plain MZ from notes/mem_A.bin + notes/unpack_info.json. Relocs are image-relative; stored values must be made load-relative (subtract 0x810)."""
import json, struct, sys
out = sys.argv[1]
i = json.load(open('notes/unpack_info.json')); A = bytearray(open('notes/mem_A.bin', 'rb').read())
LOADSEG = 0x810
rel = sorted(i['relocs']); r = i['regs']
last = max(k for k in range(len(A)) if A[k]) + 1
img = A[:last]
for o in rel:
    v = img[o] | img[o+1] << 8; v = (v - LOADSEG) & 0xFFFF
    img[o] = v & 0xFF; img[o+1] = v >> 8
hdr_paras = ((28 + 4*len(rel)) + 511) // 512 * 32   # header padded to 512 bytes
hdr = bytearray(hdr_paras*16)
total = len(hdr) + len(img)
need_top = (r['ss']*16 + r['sp'] + 15)//16               # paragraphs needed (rel to load) for stack top
img_paras = (len(img)+15)//16
minalloc = max(0, need_top - img_paras)
struct.pack_into('<14H', hdr, 0, 0x5A4D, total % 512, (total+511)//512, len(rel), hdr_paras, minalloc, 0xFFFF,
                 r['ss'], r['sp'], 0, r['ip'], r['cs'], 28, 0)
# NOTE: unpacked entry is at CS:IP = 2C79:0016 (after the INT21 hook), real entry is 2C79:0012 (mov ah,30h at 0012)
struct.pack_into('<H', hdr, 20, 0x0012)
for k, o in enumerate(rel):
    struct.pack_into('<HH', hdr, 28+4*k, o & 0xF, o >> 4)   # offset:segment form
open(out, 'wb').write(bytes(hdr) + bytes(img))
print('wrote', out, len(hdr)+len(img), 'bytes; relocs', len(rel), 'minalloc', hex(minalloc))
