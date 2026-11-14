#!/usr/bin/env python3
"""Unpack ST3.EXE (FCP protector + PKLITE) by emulation. Usage: PYTHONPATH=tools/pylib python3 tools/unpack.py ST3.EXE out.EXE
Runs the protected EXE at two load segments; first INT 21h marks the real entry. Diff of the two
memory images recovers relocations; emits plain MZ."""
import sys, struct
from unicorn import *
from unicorn.x86_const import *

def run(path, psp):
    data = open(path, 'rb').read()
    (sig, lastpg, pages, nrel, hdrpar, minalloc, maxalloc, ss, sp, csum, ip, cs, reloff, ovl) = struct.unpack('<14H', data[:28])
    assert nrel == 0
    load = psp + 0x10
    image = data[hdrpar*16:(pages-1)*512+(lastpg or 512)]
    uc = Uc(UC_ARCH_X86, UC_MODE_16); uc.mem_map(0, 0x100000)
    uc.mem_write(load*16, image)
    p = bytearray(256); p[0:2] = b'\xCD\x20'; p[0x80] = 0; p[0x81] = 0x0D
    uc.mem_write(psp*16, bytes(p))
    for n in range(256):
        uc.mem_write(n*4, struct.pack('<HH', n*4, 0xF000)); uc.mem_write(0xF0000+n*4, b'\xCF')
    R = uc.reg_read
    for r, v in [(UC_X86_REG_CS, load+cs), (UC_X86_REG_IP, ip), (UC_X86_REG_SS, load+ss), (UC_X86_REG_SP, sp),
                 (UC_X86_REG_DS, psp), (UC_X86_REG_ES, psp), (UC_X86_REG_AX, 0), (UC_X86_REG_EFLAGS, 0x202)]:
        uc.reg_write(r, v)
    st = {'maxw': 0, 'minw': 1 << 30, 'regs': None, 'n': 0}
    def push16(v):
        s_ = (R(UC_X86_REG_SP)-2) & 0xFFFF; uc.reg_write(UC_X86_REG_SP, s_)
        uc.mem_write(R(UC_X86_REG_SS)*16+s_, struct.pack('<H', v & 0xFFFF))
    def code(u, a, sz, ud):
        st['n'] += 1
    def intr(u, no, ud):
        ax = R(UC_X86_REG_AX); cs_, ip_ = R(UC_X86_REG_CS), R(UC_X86_REG_IP)
        vec = struct.unpack('<HH', bytes(u.mem_read(no*4, 4)))
        if vec[1] != 0xF000:
            fl = R(UC_X86_REG_EFLAGS) & 0xFFFF; push16(fl); push16(cs_); push16(ip_)
            u.reg_write(UC_X86_REG_EFLAGS, fl & ~0x300); u.reg_write(UC_X86_REG_CS, vec[1]); u.reg_write(UC_X86_REG_IP, vec[0]); return
        if no == 0x21:
            st['regs'] = {k: R(v) for k, v in dict(cs=UC_X86_REG_CS, ip=UC_X86_REG_IP, ss=UC_X86_REG_SS, sp=UC_X86_REG_SP,
                          ds=UC_X86_REG_DS, es=UC_X86_REG_ES, ax=UC_X86_REG_AX, bx=UC_X86_REG_BX, cx=UC_X86_REG_CX,
                          dx=UC_X86_REG_DX, si=UC_X86_REG_SI, di=UC_X86_REG_DI, bp=UC_X86_REG_BP).items()}
            u.emu_stop(); return
        if no == 0x13: u.reg_write(UC_X86_REG_AX, ax & 0xFF)
        elif no == 0x10 and (ax >> 8) == 0x0F: u.reg_write(UC_X86_REG_AX, 0x5003)
    def memw(u, acc, a, sz, val, ud):
        if a >= load*16:           # image writes (stub/decompressor self-writes are included; trimmed below)
            st['maxw'] = max(st['maxw'], a+sz); st['minw'] = min(st['minw'], a)
    uc.hook_add(UC_HOOK_CODE, code); uc.hook_add(UC_HOOK_INTR, intr)
    uc.hook_add(UC_HOOK_MEM_WRITE, memw)
    uc.emu_start(R(UC_X86_REG_CS)*16 + R(UC_X86_REG_IP), 0xFFFFF0)
    assert st['regs'], 'never reached INT 21h'
    return load, st, uc

path, out = sys.argv[1], sys.argv[2]
LA, LB = 0x0800, 0x1800
loadA, sa, ua = run(path, LA)
loadB, sb, ub = run(path, LB)
r = sa['regs']; print('entry regs A:', {k: hex(v) for k, v in r.items()}, 'insns', sa['n'])
entry_cs, entry_ip = r['cs']-loadA, r['ip']
print('entry rel CS:IP = %04X:%04X   SS:SP rel = %04X:%04X' % (entry_cs, entry_ip, r['ss']-loadA, r['sp']))
# Image size: PKLITE-decompressed image = up to the decompressor/stub area. Use diff-extent over a generous window.
WIN = 0x44000
A = bytes(ua.mem_read(loadA*16, WIN)); B = bytes(ub.mem_read(loadB*16, WIN))
rel = []; diffbytes = 0
for off in range(0, WIN-1):
    wa = A[off] | A[off+1] << 8; wb = B[off] | B[off+1] << 8
    if wa != wb:
        if (wb - wa) & 0xFFFF == (LB-LA):
            rel.append(off)
        else:
            diffbytes += 1
print('relocs found', len(rel), 'other-diff bytes', diffbytes)
open('notes/mem_A.bin', 'wb').write(A)
import json; json.dump({'regs': {k: v-loadA if k in ('cs','ss') else v for k, v in r.items()}, 'maxw_rel': sa['maxw']-loadA*16, 'relocs': rel}, open('notes/unpack_info.json', 'w'))
