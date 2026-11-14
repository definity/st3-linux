#!/usr/bin/env python3
"""Unicorn-based tracer for ST3.EXE (16-bit real mode). Milestone 1 tool.
Usage: PYTHONPATH=tools/pylib python3 tools/trace.py [exe] [max_insns]
Logs: first-time-executed instructions, INTs, IVT writes. Minimal DOS/BIOS HLE.
"""
import sys, struct
from unicorn import *
from unicorn.x86_const import *
from capstone import *

EXE = sys.argv[1] if len(sys.argv) > 1 else 'ST3.EXE'
MAXI = int(sys.argv[2]) if len(sys.argv) > 2 else 3_000_000
import os; STOP21 = bool(os.environ.get('STOP21'))
PSP = 0x0800
LOAD = PSP + 0x10
MEMSZ = 0x100000

data = open(EXE, 'rb').read()
(sig, lastpg, pages, nrel, hdrpar, minalloc, maxalloc, ss, sp, csum, ip, cs, reloff, ovl) = struct.unpack('<14H', data[:28])
image = data[hdrpar*16:(pages-1)*512+(lastpg or 512)]

uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, MEMSZ)
uc.mem_write(LOAD*16, image)
for i in range(nrel):
    o, s = struct.unpack('<HH', data[reloff+i*4:reloff+i*4+4])
    a = (LOAD+s)*16+o
    v = struct.unpack('<H', bytes(uc.mem_read(a, 2)))[0]
    uc.mem_write(a, struct.pack('<H', (v+LOAD) & 0xFFFF))
# PSP
psp = bytearray(256); psp[0:2] = b'\xCD\x20'; struct.pack_into('<H', psp, 2, 0xA000)
psp[0x80] = 0; psp[0x81] = 0x0D
uc.mem_write(PSP*16, bytes(psp))
# BIOS stubs: each IVT entry -> F000:(n*4) containing IRET marker; we HLE all INTs by hook anyway
for n in range(256):
    uc.mem_write(n*4, struct.pack('<HH', n*4, 0xF000))
    uc.mem_write(0xF0000+n*4, b'\xCF')
uc.reg_write(UC_X86_REG_CS, LOAD+cs); uc.reg_write(UC_X86_REG_IP, ip)
uc.reg_write(UC_X86_REG_SS, LOAD+ss); uc.reg_write(UC_X86_REG_SP, sp)
uc.reg_write(UC_X86_REG_DS, PSP); uc.reg_write(UC_X86_REG_ES, PSP)
uc.reg_write(UC_X86_REG_AX, 0)
uc.reg_write(UC_X86_REG_EFLAGS, 0x202)

md = Cs(CS_ARCH_X86, CS_MODE_16)
seen = set(); icount = 0
log = open('notes/trace.log', 'w')
def L(*a):
    print(*a, file=log)

def lin(): return uc.reg_read(UC_X86_REG_CS)*16 + uc.reg_read(UC_X86_REG_IP)
def rd(r): return uc.reg_read(r)

def push16(v):
    sp_ = (rd(UC_X86_REG_SP)-2) & 0xFFFF
    uc.reg_write(UC_X86_REG_SP, sp_)
    uc.mem_write(rd(UC_X86_REG_SS)*16+sp_, struct.pack('<H', v & 0xFFFF))

def hook_code(u, addr, size, ud):
    global icount
    icount += 1
    if icount > MAXI:
        L('MAX INSNS'); u.emu_stop(); return
    if addr in seen: return
    seen.add(addr)
    if len(seen) > 4000: return
    code = bytes(u.mem_read(addr, min(size, 15)))
    for i in md.disasm(code, addr, 1):
        cs_ = rd(UC_X86_REG_CS)
        L(f'{cs_-LOAD:04X}:{rd(UC_X86_REG_IP):04X} {code.hex():14s} {i.mnemonic} {i.op_str}'
          f'   ; AX={rd(UC_X86_REG_AX):04X} BX={rd(UC_X86_REG_BX):04X} CX={rd(UC_X86_REG_CX):04X} DX={rd(UC_X86_REG_DX):04X}')

def hook_intr(u, intno, ud):
    ax = rd(UC_X86_REG_AX); ah = ax >> 8
    cs_, ip_ = rd(UC_X86_REG_CS), rd(UC_X86_REG_IP)
    vec = struct.unpack('<HH', bytes(u.mem_read(intno*4, 4)))
    if intno != 1: L(f'  INT {intno:02X} AX={ax:04X} BX={rd(UC_X86_REG_BX):04X} CX={rd(UC_X86_REG_CX):04X} DX={rd(UC_X86_REG_DX):04X} from {cs_-LOAD:04X}:{ip_:04X} vec={vec[1]:04X}:{vec[0]:04X}')
    if vec[1] != 0xF000:   # program-installed handler: do a real software interrupt
        fl = rd(UC_X86_REG_EFLAGS) & 0xFFFF
        push16(fl); push16(cs_); push16(ip_)
        u.reg_write(UC_X86_REG_EFLAGS, fl & ~0x300)
        u.reg_write(UC_X86_REG_CS, vec[1]); u.reg_write(UC_X86_REG_IP, vec[0])
        return
    # HLE
    if intno == 0x21 and STOP21:
        L(f'  FIRST INT21 at {cs_-LOAD:04X}:{ip_:04X} AX={ax:04X} -> stopping'); print('first INT21 at',hex(cs_-LOAD),hex(ip_),hex(ax)); u.emu_stop(); return
    if intno == 0x21:
        if ah == 0x30: u.reg_write(UC_X86_REG_AX, 0x0005)
        elif ah == 0x4C: L(f'  EXIT code {ax&0xFF}'); u.emu_stop()
        elif ah == 0x09:
            a = rd(UC_X86_REG_DS)*16 + rd(UC_X86_REG_DX); s = b''
            while True:
                b = bytes(u.mem_read(a, 1))
                if b == b'$': break
                s += b; a += 1
            L('  PRINT', s); print(s.decode('cp437', 'replace'), end='')
        elif ah == 0x25:
            a = rd(UC_X86_REG_DS), rd(UC_X86_REG_DX)
            u.mem_write((ax & 0xFF)*4, struct.pack('<HH', a[1], a[0]))
        elif ah == 0x35:
            o, s = struct.unpack('<HH', bytes(u.mem_read((ax & 0xFF)*4, 4)))
            u.reg_write(UC_X86_REG_BX, o); u.reg_write(UC_X86_REG_ES, s)
    elif intno == 0x13: u.reg_write(UC_X86_REG_AX, ax & 0xFF)  # report error-ish
    elif intno == 0x10 and ah == 0x0F: u.reg_write(UC_X86_REG_AX, 0x5003)  # color 80x25
    # others: no-op

def hook_mem_ivt(u, access, addr, size, value, ud):
    L(f'  IVT WRITE [{addr:04X}] size={size} val={value:X} at {rd(UC_X86_REG_CS)-LOAD:04X}:{rd(UC_X86_REG_IP):04X}')

uc.hook_add(UC_HOOK_CODE, hook_code)
uc.hook_add(UC_HOOK_INTR, hook_intr)
uc.hook_add(UC_HOOK_MEM_WRITE, hook_mem_ivt, begin=0, end=0x3FF)

try:
    uc.emu_start(lin(), 0xFFFFF0)
except UcError as e:
    L('UC ERROR', e, f'at {rd(UC_X86_REG_CS)-LOAD:04X}:{rd(UC_X86_REG_IP):04X}')
    print('UC ERROR', e)
L('insns executed', icount, 'unique', len(seen))
log.close()
print('done; insns', icount, 'unique', len(seen))
open('notes/mem_after.bin', 'wb').write(bytes(uc.mem_read(LOAD*16, 0x30000)))
