#!/usr/bin/env python3
"""Differential fuzzer: st3emu cpu.c vs Unicorn (QEMU) — one random real-mode instruction per case.
Usage (from st3emu/): PYTHONPATH=../tools/pylib python3 tests/fuzz_cpu.py [cases] [seed] [only_prefix_hex]
Compares GPRs, segment regs, IP, flags (minus architecturally-undefined flags per instruction), and memory."""
import sys, os, ctypes, random, struct, collections
from unicorn import *
from unicorn.x86_const import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

N = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
ONLY = bytes.fromhex(sys.argv[3]) if len(sys.argv) > 3 else None
rng = random.Random(SEED)

lib = ctypes.CDLL(os.path.abspath('libst3cpu.so'))
lib.t_mem.restype = ctypes.POINTER(ctypes.c_uint8)
lib.t_init()
MEMSZ = 0x110000
cmem = (ctypes.c_uint8 * MEMSZ).from_address(ctypes.addressof(lib.t_mem().contents))

uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x200000)
md = Cs(CS_ARCH_X86, CS_MODE_16)

GPR = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
SEGR = [UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_FS, UC_X86_REG_GS]
SEGS = [0x3000, 0x1000, 0x4000, 0x2000, 0x5000, 0x6000]
CMP_LO, CMP_HI = 0x0000, 0x80000

# I/O: same deterministic function as cpu_api.c
def io_val(port, size):
    v = ((port * 0x9E3779B1) ^ 0x5A5A5A5A) & 0xFFFFFFFF
    return v & (0xFF if size == 1 else 0xFFFF if size == 2 else 0xFFFFFFFF)
uc.hook_add(UC_HOOK_INSN, lambda u, port, size, ud: io_val(port, size), None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, lambda u, port, size, val, ud: None, None, 1, 0, UC_X86_INS_OUT)

state = {'intr': None}
def uc_push16(v):
    sp = (uc.reg_read(UC_X86_REG_SP) - 2) & 0xFFFF; uc.reg_write(UC_X86_REG_SP, sp)
    uc.mem_write(uc.reg_read(UC_X86_REG_SS) * 16 + sp, struct.pack('<H', v & 0xFFFF))
def hook_intr(u, no, ud):
    fl = u.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF
    cs, ip = u.reg_read(UC_X86_REG_CS), u.reg_read(UC_X86_REG_IP)
    uc_push16(fl | 2); uc_push16(cs); uc_push16(ip)
    u.reg_write(UC_X86_REG_EFLAGS, fl & ~0x300)
    vec = struct.unpack('<HH', bytes(u.mem_read(no * 4, 4)))
    u.reg_write(UC_X86_REG_CS, vec[1]); u.reg_write(UC_X86_REG_IP, vec[0])
    state['intr'] = no
    u.emu_stop()
uc.hook_add(UC_HOOK_INTR, hook_intr)

def randomize_memory():
    blob = bytes(rng.getrandbits(8) for _ in range(CMP_HI - CMP_LO))
    uc.mem_write(CMP_LO, blob)
    ctypes.memmove(ctypes.addressof(cmem) + CMP_LO, blob, len(blob))

# ---- instruction templates ----
ONE_BYTE = [b for b in range(0x100) if b not in (
    0x0F, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3,   # prefixes handled separately
    0x63, 0xF1, 0xF4, 0x62,                                                     # arpl, icebp, hlt, bound
    0x8E, 0x17, 0x07, 0x1F,                                                     # seg loads tested separately (random segs break later cases)
    0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0x9B,                       # FPU (we emulate "no FPU")
    0x9A, 0xEA, 0xCA, 0xCB, 0xCF,                                               # far control flow tested with fixed targets
    0x6C, 0x6D, 0x6E, 0x6F,                                                     # ins/outs (callbacks)
    0xC4, 0xC5, 0x8C,                                                           # les/lds (seg loads), mov r/m,sreg (seg>5 UD)
    0x82)]                                                                      # 82 alias: QEMU accepts in 16-bit? skip
TWO_BYTE = list(range(0x80, 0xA0)) + [0xA3, 0xA4, 0xA5, 0xAB, 0xAC, 0xAD, 0xAF, 0xB3, 0xB6, 0xB7, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF]

def gen():
    pre = b''
    if rng.random() < 0.35: pre += b'\x66'
    if rng.random() < 0.15: pre += rng.choice([b'\x26', b'\x2e', b'\x36', b'\x3e', b'\x64', b'\x65'])
    a32 = rng.random() < 0.10
    if a32: pre += b'\x67'
    if rng.random() < 0.85:
        op = bytes([rng.choice(ONE_BYTE)])
        if op[0] in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF) and rng.random() < 0.5:
            pre = rng.choice([b'\xf3', b'\xf2']) + pre
    else:
        op = b'\x0f' + bytes([rng.choice(TWO_BYTE)])
    tail = bytes(rng.getrandbits(8) for _ in range(10))
    return pre + op + tail, a32

# undefined-flag masks by mnemonic (bits to IGNORE)
CF, PF, AF, ZF, SF, OF = 1, 4, 0x10, 0x40, 0x80, 0x800
UNDEF = collections.defaultdict(int, {
    'and': AF, 'or': AF, 'xor': AF, 'test': AF,
    'mul': SF | ZF | AF | PF, 'imul': SF | ZF | AF | PF,
    'div': CF | PF | AF | ZF | SF | OF, 'idiv': CF | PF | AF | ZF | SF | OF,
    'daa': OF, 'das': OF, 'aaa': SF | ZF | PF | OF, 'aas': SF | ZF | PF | OF,
    'aam': CF | OF | AF, 'aad': CF | OF | AF,
    'bsf': CF | PF | AF | SF | OF, 'bsr': CF | PF | AF | SF | OF,
    'bt': OF | SF | AF | PF | ZF, 'bts': OF | SF | AF | PF | ZF, 'btr': OF | SF | AF | PF | ZF, 'btc': OF | SF | AF | PF | ZF,
    'shl': AF, 'sal': AF, 'shr': AF, 'sar': AF, 'shld': AF, 'shrd': AF,
})
SHIFTS = ('rol', 'ror', 'rcl', 'rcr', 'shl', 'sal', 'shr', 'sar', 'shld', 'shrd')

def reg_state_c():
    r = (ctypes.c_uint32 * 8)(); s = (ctypes.c_uint16 * 6)(); e = ctypes.c_uint32(); f = ctypes.c_uint32()
    lib.t_get(r, s, ctypes.byref(e), ctypes.byref(f))
    return list(r), list(s), e.value & 0xFFFF, f.value
def reg_state_u():
    return [uc.reg_read(x) for x in GPR], [uc.reg_read(x) for x in SEGR], uc.reg_read(UC_X86_REG_IP), uc.reg_read(UC_X86_REG_EFLAGS)

stats = collections.Counter(); fails = collections.Counter(); examples = {}
randomize_memory()
for case in range(N):
    if case % 500 == 0 and case: randomize_memory()
    code, a32 = gen()
    if ONLY and not code.startswith(ONLY): continue
    regs = [rng.getrandbits(32) for _ in range(8)]
    if a32 or rng.random() < 0.5: regs = [x & 0xFFFF for x in regs]
    regs[4] = (regs[4] & 0xFFFF) | 0x100   # keep SP away from 0 wrap mostly
    regs[1] = (regs[1] & 0xFFFF0000) | (rng.choice([0, 1, 2, 3, 5, 31, 32, 33, 8, 16, 17]) if rng.random() < 0.5 else regs[1] & 0xFFFF)
    fl = (rng.getrandbits(16) & 0x0CD5) | 0x0002 | 0x0200   # CF PF AF ZF SF DF OF random, IF=1, TF=0
    ip = rng.randrange(0x100, 0xF000)
    # place code in both memories
    cl = SEGS[1] * 16 + ip
    uc.mem_write(cl, code); ctypes.memmove(ctypes.addressof(cmem) + cl, code, len(code))
    # set state
    for i, x in enumerate(GPR): uc.reg_write(x, regs[i])
    for i, x in enumerate(SEGR): uc.reg_write(x, SEGS[i])
    uc.reg_write(UC_X86_REG_EIP, ip); uc.reg_write(UC_X86_REG_EFLAGS, fl)
    lib.t_set((ctypes.c_uint32 * 8)(*regs), (ctypes.c_uint16 * 6)(*SEGS), ip, fl)
    state['intr'] = None
    ins = next(md.disasm(code, 0, 1), None)
    mn = ins.mnemonic if ins else '??'
    # run unicorn
    uerr = None
    try:
        uc.ctl_remove_cache(cl & ~0xFFF, (cl & ~0xFFF) + 0x2000)
        if ins and ins.mnemonic.startswith('rep'):
            for _ in range(200):
                uc.emu_start(uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP), cl + ins.size)
                if uc.reg_read(UC_X86_REG_IP) == (ip + ins.size) & 0xFFFF or state['intr'] is not None: break
        else:
            uc.emu_start(cl, 0x1FFFFF, count=1)
    except UcError as e:
        uerr = e
    cret = lib.t_step()
    if uerr is not None:
        stats['uc_err'] += 1
        # restore memories to identical state
        if cret == 0: fails['C accepts, UC rejects:' + mn] += 1; examples.setdefault('C accepts, UC rejects:' + mn, code.hex())
        randomize_memory(); continue
    if state['intr'] == 0:   # Unicorn drops reg writes made in the hook for CPU exceptions (#DE) -> can't compare
        stats['skip_de'] += 1; randomize_memory(); continue
    if cret != 0:
        fails['C unimpl:' + mn] += 1; examples.setdefault('C unimpl:' + mn, code.hex()); randomize_memory(); continue
    cr, cs, cip, cf = reg_state_c(); ur, us, uip, uf = reg_state_u()
    mask = 0xFD5 & ~UNDEF[mn]
    if mn in SHIFTS:
        mask &= ~OF   # OF defined only for count==1; be lenient
    if mn in ('shld', 'shrd') and ins and 'word ptr' in ins.op_str or (mn in ('shld','shrd') and ins and ins.op_str.split(',')[0].strip() in ('ax','bx','cx','dx','si','di','bp','sp')):
        cnt = (regs[1] & 0xFF if ins.op_str.endswith('cl') else int(ins.op_str.split(',')[-1], 0)) & 31
        if cnt > 16: stats['skip_undef'] += 1; randomize_memory(); continue
    bad = []
    for i in range(8):
        if cr[i] != ur[i]:
            if mn in ('bsf', 'bsr') and (uf & ZF): continue
            bad.append(f'r{i} C={cr[i]:08X} U={ur[i]:08X}')
    for i in range(6):
        if cs[i] != us[i]: bad.append(f'seg{i} C={cs[i]:04X} U={us[i]:04X}')
    if cip != uip: bad.append(f'ip C={cip:04X} U={uip:04X}')
    if (cf ^ uf) & mask: bad.append(f'flags C={cf:04X} U={uf:04X} diff={(cf ^ uf) & mask:04X}')
    cm = bytes(cmem[CMP_LO:CMP_HI]); um = bytes(uc.mem_read(CMP_LO, CMP_HI - CMP_LO))
    if cm != um:
        diffs = [i for i in range(len(cm)) if cm[i] != um[i]]
        bad.append(f'mem {len(diffs)} bytes differ first@{CMP_LO+diffs[0]:05X} C={cm[diffs[0]]:02X} U={um[diffs[0]]:02X}')
        randomize_memory()
    stats['ok' if not bad else 'bad'] += 1
    if bad:
        key = mn
        fails[key] += 1
        if key not in examples:
            examples[key] = f'{code.hex()}  [{ins.mnemonic if ins else "?"} {ins.op_str if ins else ""}] regs={["%08X" % x for x in regs]} fl={fl:04X}\n      ' + '; '.join(bad)
print('cases', N, dict(stats))
for k, v in fails.most_common(60):
    print(f'{v:6d} {k}\n      {examples.get(k, "")}')
