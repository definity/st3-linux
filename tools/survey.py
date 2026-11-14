#!/usr/bin/env python3
"""Milestone-2 survey harness: run unpacked ST3U.EXE under Unicorn with a DOS/BIOS HLE layer.
Goals: log every I/O port and INT service the program uses, deliver timer IRQs, dump the text screen.
This is a prototype of the eventual C emulator's HLE layer, not the product.

Usage (from work/):  PYTHONPATH=tools/pylib python3 tools/survey.py [--insns N] [--keys "..."] [--env K=V ...] [-- args]
Runs inside work/sandbox (DOS C:\\ST3 == sandbox/). Outputs notes/survey_*.txt
"""
import sys, os, struct, time, collections, argparse
from unicorn import *
from unicorn.x86_const import *

ap = argparse.ArgumentParser()
ap.add_argument('--insns', type=int, default=200_000_000)
ap.add_argument('--keys', default='')          # scripted keystrokes (ascii); delivered after idle
ap.add_argument('--env', action='append', default=[])
ap.add_argument('--mips', type=float, default=20.0)   # virtual CPU speed for PIT timing
ap.add_argument('--tag', default='run')
ap.add_argument('--census', type=int, default=0)   # start opcode census at this insn count (0=off)
ap.add_argument('--census-len', type=int, default=3_000_000)
ap.add_argument('args', nargs='*')
A = ap.parse_args()

ROOT = os.path.abspath('sandbox')
EXE = os.path.join(ROOT, 'ST3U.EXE')
OUT = os.path.abspath('notes')
log = open(f'{OUT}/survey_{A.tag}.log', 'w')
def L(*a): print(*a, file=log); log.flush()

uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x110000)   # 1MB + HMA
R = uc.reg_read; W = uc.reg_write
def rb(a): return uc.mem_read(a, 1)[0]
def rw(a): return struct.unpack('<H', bytes(uc.mem_read(a, 2)))[0]
def wb(a, v): uc.mem_write(a, bytes([v & 0xFF]))
def ww(a, v): uc.mem_write(a, struct.pack('<H', v & 0xFFFF))
def asciiz(a, mx=256):
    b = bytes(uc.mem_read(a, mx)); return b.split(b'\0')[0].decode('cp437')

CF, ZF = 1, 0x40
# ---------------- BIOS / IVT setup ----------------
STUBSEG = 0xF000
for n in range(256):
    ww(n*4, n*4 * 0 + n); ww(n*4+2, STUBSEG)           # vector n -> F000:n  (1-byte stubs)
    wb(STUBSEG*16 + n, 0xCF)                           # IRET (never actually executed; trapped)
bda = 0x400
ww(bda+0x10, 0x0021 | 0x0002)   # equipment: 80x25 color, FPU? no
ww(bda+0x13, 640)
wb(bda+0x49, 3); ww(bda+0x4A, 80); ww(bda+0x4C, 4096); ww(bda+0x63, 0x3D4)
wb(bda+0x84, 24); ww(bda+0x85, 16)
ww(bda+0x1A, 0x1E); ww(bda+0x1C, 0x1E); ww(bda+0x80, 0x1E); ww(bda+0x82, 0x3E)
uc.mem_write(0xFFFF5, b'12/25/94')   # BIOS date
wb(0xFFFFE, 0xFC)                    # model byte AT

# ---------------- DOS memory / PSP / env ----------------
ENVSEG, PSP = 0x0700, 0x0800
envs = ['COMSPEC=C:\\COMMAND.COM', 'PATH=C:\\'] + A.env
eb = b''.join(e.encode() + b'\0' for e in envs) + b'\0' + b'\x01\x00' + b'C:\\ST3\\ST3.EXE\0'
uc.mem_write(ENVSEG*16, eb)
TOP = 0xA000
data = open(EXE, 'rb').read()
(sig, lastpg, pages, nrel, hdrpar, minalloc, maxalloc, ss, sp, csum, ip, cs, reloff, ovl) = struct.unpack('<14H', data[:28])
LOAD = PSP + 0x10
img = data[hdrpar*16:(pages-1)*512 + (lastpg or 512)]
uc.mem_write(LOAD*16, img)
for i in range(nrel):
    o, s = struct.unpack('<HH', data[reloff+i*4:reloff+i*4+4]); a = (LOAD+s)*16 + o
    ww(a, rw(a) + LOAD)
psp = bytearray(256); psp[0:2] = b'\xCD\x20'; struct.pack_into('<H', psp, 2, TOP)
struct.pack_into('<H', psp, 0x2C, ENVSEG)
psp[0x18:0x2C] = bytes([1, 1, 1, 0, 2] + [0xFF]*15); struct.pack_into('<HHH', psp, 0x32, 20, 0x18, PSP)
cmd = (' ' + ' '.join(A.args)).encode() if A.args else b''
psp[0x80] = len(cmd); psp[0x81:0x81+len(cmd)] = cmd; psp[0x81+len(cmd)] = 0x0D
uc.mem_write(PSP*16, bytes(psp))
dta = [PSP, 0x80]

FIRST_MCB = ENVSEG - 1
def mcb_w(m, sig, owner, size):
    wb(m*16, sig); ww(m*16+1, owner); ww(m*16+3, size)
def mcb_r(m):
    if not (0 <= m < TOP): return 0, 0xFFFF, 0
    return rb(m*16), rw(m*16+1), rw(m*16+3)
mcb_w(FIRST_MCB, 0x4D, PSP, PSP - 1 - ENVSEG)          # env block
mcb_w(PSP - 1, 0x5A, PSP, TOP - PSP)                   # program block gets everything
def mcb_walk():
    m = FIRST_MCB; out = []
    while True:
        sig, own, sz = mcb_r(m); out.append((m, sig, own, sz))
        if sig != 0x4D or len(out) > 2000: return out
        m += sz + 1
def mcb_merge():
    m = FIRST_MCB
    while True:
        sig, own, sz = mcb_r(m)
        if sig != 0x4D: return
        n = m + sz + 1; nsig, nown, nsz = mcb_r(n)
        if own == 0 and nown == 0:
            mcb_w(m, nsig, 0, sz + nsz + 1); continue
        m = n
def max_free():
    return max([sz for m, sig, own, sz in mcb_walk() if own == 0] + [0])
def mcb_split(m, paras):
    sig, own, sz = mcb_r(m)
    if sz > paras:
        mcb_w(m + paras + 1, sig, 0, sz - paras - 1); mcb_w(m, 0x4D, own, paras)
def dos_alloc(paras):
    mcb_merge()
    for m, sig, own, sz in mcb_walk():
        if own == 0 and sz >= paras:
            mcb_split(m, paras); mcb_w(m, mcb_r(m)[0], PSP, paras); return m + 1, 0
    return None, max_free()
def dos_free(seg):
    sig, own, sz = mcb_r(seg - 1)
    if sig not in (0x4D, 0x5A): return False
    mcb_w(seg - 1, sig, 0, sz); mcb_merge(); return True
def dos_resize(seg, paras):
    m = seg - 1; sig, own, sz = mcb_r(m)
    if sig not in (0x4D, 0x5A): return False, 0
    # absorb following free blocks
    while sig == 0x4D:
        n = m + sz + 1; nsig, nown, nsz = mcb_r(n)
        if nown != 0: break
        sz = sz + nsz + 1; sig = nsig; mcb_w(m, sig, own, sz)
    if sz < paras: return False, sz
    mcb_split(m, paras); return True, paras

# ---------------- file system ----------------
cwd = ''   # relative to ROOT, DOS-style components
handles = {0: 'stdin', 1: 'stdout', 2: 'stderr', 3: 'aux', 4: 'prn'}
def host_path(dospath):
    p = dospath.replace('/', '\\')
    if len(p) >= 2 and p[1] == ':': p = p[2:]
    if p.startswith('\\'):
        parts = [x for x in p.split('\\') if x]
        if parts and parts[0].upper() == 'ST3': parts = parts[1:]
    else:
        parts = [x for x in cwd.split('\\') if x] + [x for x in p.split('\\') if x]
    cur = ROOT
    for comp in parts:
        if comp == '.': continue
        if comp == '..': cur = os.path.dirname(cur) if cur != ROOT else ROOT; continue
        try:
            m = [e for e in os.listdir(cur) if e.upper() == comp.upper()]
        except OSError: m = []
        cur = os.path.join(cur, m[0] if m else comp.upper())
    return cur
finds = {}
def fnmatch83(name, pat):
    def split(s):
        s = s.upper(); b, _, e = s.partition('.'); return b, e
    nb, ne = split(name); pb, pe = split(pat)
    def m(n, p, ln):
        n = n.ljust(ln); p2 = ''
        for ch in p:
            if ch == '*': p2 += '?' * (ln - len(p2)); break
            p2 += ch
        p2 = p2.ljust(ln)[:ln]
        return all(pc == '?' or pc == nc for pc, nc in zip(p2, n[:ln]))
    if '.' not in pat.upper() and pat.endswith('*'): pe = '*'
    return m(nb, pb, 8) and m(ne, pe, 3)
def dos_time(t):
    lt = time.localtime(t)
    return (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2), ((lt.tm_year-1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
def fill_dta(entry):
    path, name = entry; st = os.stat(path)
    a = dta[0]*16 + dta[1]
    t, d = dos_time(st.st_mtime)
    wb(a+21, 0x10 if os.path.isdir(path) else 0x20); ww(a+22, t); ww(a+24, d)
    uc.mem_write(a+26, struct.pack('<I', min(st.st_size, 0xFFFFFFFF)))
    nm = name.upper().encode()[:12] + b'\0'; uc.mem_write(a+30, nm.ljust(13, b'\0'))

# ---------------- statistics ----------------
ports = collections.Counter(); port_first = {}
ints = collections.Counter(); int_first = {}
screen_out = []
state = {'n': 0, 'pending_stub': None, 'stop': None, 'pit_div': 65536, 'pic_mask': 0xB8, 'isr': 0,
         'vtime': 0.0, 'next_tick': 0.0, 'r3da': 0, 'keys': [], 'idle': 0, 'kbq': [], 'last60': 0,
         'pit_mode_writes': [], 'dsp': [], 'sb_reset': 0}

def where(): return f'{R(UC_X86_REG_CS)-LOAD:04X}:{R(UC_X86_REG_IP):04X}'

def push16(v):
    s_ = (R(UC_X86_REG_SP)-2) & 0xFFFF; W(UC_X86_REG_SP, s_); ww(R(UC_X86_REG_SS)*16 + s_, v)
def pop16():
    s_ = R(UC_X86_REG_SP); v = rw(R(UC_X86_REG_SS)*16 + s_); W(UC_X86_REG_SP, (s_+2) & 0xFFFF); return v

def do_int(no, cs_=None, ip_=None):
    """Real-mode interrupt vectoring through the IVT."""
    if cs_ is None: cs_, ip_ = R(UC_X86_REG_CS), R(UC_X86_REG_IP)
    fl = R(UC_X86_REG_EFLAGS) & 0xFFFF
    push16(fl); push16(cs_); push16(ip_)
    W(UC_X86_REG_EFLAGS, fl & ~0x300)
    W(UC_X86_REG_CS, rw(no*4+2)); W(UC_X86_REG_IP, rw(no*4))

def hook_intr(u, no, ud):
    do_int(no)

def hook_stub(u, addr, size, ud):
    state['pending_stub'] = addr - STUBSEG*16
    u.emu_stop()

# ---- port I/O ----
def io_in(u, port, size, ud):
    key = ('in', port); ports[key] += 1; port_first.setdefault(key, where())
    if port == 0x3DA:
        state['attr_flip'] = 0
        state['r3da'] ^= 1
        return 0x09 if state['r3da'] else 0x00
    if port == 0x21: return state['pic_mask']
    if port <= 0x0F or 0x80 <= port <= 0x8F: return dma_in(port)
    if SB_BASE <= port <= SB_BASE + 0xF: return sb_in(port - SB_BASE)
    if port == 0x60: return state['last60']
    if port == 0x64: return 0x14 | (1 if state['kbq'] else 0)
    if port == 0x61: return 0x20 if (state['n'] // 50) & 1 else 0x00
    if port in (0x40, 0x41, 0x42):
        return int((-state['n'] * 1.193182 / A.mips) % 65536) & 0xFF
    return 0xFF if size == 1 else 0xFFFF
def io_out(u, port, size, value, ud):
    key = ('out', port); ports[key] += 1; port_first.setdefault(key, where())
    if 0x3C0 <= port <= 0x3CF:
        vl = state.setdefault('vgalog', [])
        if len(vl) < 400: vl.append(f'{where()} out{8*size} {port:03X},{value:0{2*size}X}')
    if port == 0x3C8: state['dac_i'] = value * 3
    elif port == 0x3C9:
        DAC[state['dac_i'] % 768] = value & 0x3F; state['dac_i'] += 1
    elif port == 0x3C0:
        if state.get('attr_flip', 0) == 0: state['attr_idx'] = value & 0x1F; state['attr_flip'] = 1
        else: ATTR[state['attr_idx']] = value; state['attr_flip'] = 0
    if port in (0x3C4, 0x3CE) and size == 2:
        regs = state.setdefault('seq' if port == 0x3C4 else 'gc', {})
        regs[value & 0xFF] = value >> 8
        if port == 0x3C4 and (value & 0xFF) == 2: vga_mapmask(value >> 8)
    elif port in (0x3C5, 0x3CF):
        pass
    if port <= 0x0F or 0x80 <= port <= 0x8F: dma_out(port, value & 0xFF)
    elif SB_BASE <= port <= SB_BASE + 0xF: sb_out(port - SB_BASE, value & 0xFF)
    if port == 0x20 and (value & 0xE0) == 0x20:   # EOI: clear highest-priority in-service
        for b in range(8):
            if state['isr'] & (1 << b): state['isr'] &= ~(1 << b); break
    elif port == 0x21: state['pic_mask'] = value
    elif port == 0x43:
        state['pit_mode_writes'].append(value); state['pit_lohi'] = []
    elif port == 0x40:
        state.setdefault('pit_lohi', []).append(value)
        if len(state['pit_lohi']) == 2:
            d = state['pit_lohi'][0] | state['pit_lohi'][1] << 8
            state['pit_div'] = d or 65536; state['pit_lohi'] = []
            L(f'  PIT ch0 divisor = {d} ({1193182/(d or 65536):.2f} Hz) at {where()}')
import json as _json
DAC = bytearray(768)
_ega = [0,1,2,3,4,5,0x14,7,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F]
for _i in range(64):   # default VGA DAC for EGA indices (rgbRGB)
    r = ((_i >> 2) & 1) * 2 + ((_i >> 5) & 1); g = ((_i >> 1) & 1) * 2 + ((_i >> 4) & 1); b = (_i & 1) * 2 + ((_i >> 3) & 1)
    DAC[_i*3:_i*3+3] = bytes([r*21, g*21, b*21])
ATTR = bytearray(_ega + [0x0C, 0, 0x0F, 0x08, 0])   # 0x10 mode ctl, 0x12 plane enable, 0x14 color select
FONT = bytearray(0x8000)   # plane 2 window as seen at B800 (32 bytes/glyph, 256 glyphs = 8K per font block)
state['fontmode'] = False; state['font_uploads'] = 0
def vga_mapmask(mm):
    if mm == 0x04 and not state['fontmode']:
        state['saved_text'] = bytes(uc.mem_read(0xB8000, 0x8000))
        uc.mem_write(0xB8000, bytes(FONT)); state['fontmode'] = True
    elif mm != 0x04 and state['fontmode']:
        FONT[:] = uc.mem_read(0xB8000, 0x8000)
        uc.mem_write(0xB8000, state['saved_text']); state['fontmode'] = False
        state['font_uploads'] += 1
        if state['font_uploads'] <= 3 or state['font_uploads'] % 100 == 0:
            L(f'  FONT upload #{state["font_uploads"]} at {where()} n={state["n"]}')
# ---------------- 8237 DMA + Sound Blaster Pro model ----------------
SB_BASE, SB_IRQ, SB_DMA = 0x220, 7, 1
dma = [{'addr': 0, 'cnt': 0, 'page': 0, 'mode': 0, 'mask': 1, 'cur0': 0} for _ in range(4)]
dstate = {'ff': 0}
PAGEPORT = {0x87: 0, 0x83: 1, 0x81: 2, 0x82: 3}
sb = {'rst': 0, 'outq': [], 'cmd': None, 'args': [], 'tc': 0, 'blk': 0, 'spk': 0, 'run': None, 'mix': {}, 'mixidx': 0,
      'cmdlog': [], 'irq_req': False, 'wav': bytearray(), 'blocks': 0}
DSP_ARGS = {0x10: 1, 0x14: 2, 0x16: 2, 0x17: 2, 0x1C: 0, 0x1F: 0, 0x40: 1, 0x41: 2, 0x42: 2, 0x48: 2, 0x74: 2, 0x75: 2,
            0x76: 2, 0x77: 2, 0x80: 2, 0x90: 0, 0x91: 0, 0xD0: 0, 0xD1: 0, 0xD3: 0, 0xD4: 0, 0xD8: 0, 0xDA: 0, 0xE0: 1,
            0xE1: 0, 0xE2: 1, 0xE3: 0, 0xE4: 1, 0xE8: 0, 0xF2: 0, 0x20: 0, 0x24: 2, 0x2C: 0, 0x98: 0, 0x99: 0}
def vt(): return state['n'] / (A.mips * 1e6)
def dma_pos(ch):
    """bytes transferred so far in current DMA cycle (driven by SB playback clock)"""
    r = sb['run']
    if ch != SB_DMA or not r: return 0
    tot = dma[ch]['cnt'] + 1
    done = int((vt() - r['t0']) * r['bps'])
    return done % tot if (dma[ch]['mode'] & 0x10) else min(done, tot)
def dma_out(port, v):
    if port <= 7:
        ch = port >> 1; key = 'cnt' if port & 1 else 'addr'
        if dstate['ff'] == 0: dma[ch][key] = (dma[ch][key] & 0xFF00) | v
        else: dma[ch][key] = (dma[ch][key] & 0x00FF) | (v << 8)
        dstate['ff'] ^= 1
    elif port == 0x0A: dma[v & 3]['mask'] = (v >> 2) & 1
    elif port == 0x0B: dma[v & 3]['mode'] = v
    elif port == 0x0C: dstate['ff'] = 0
    elif port in PAGEPORT: dma[PAGEPORT[port]]['page'] = v
    if len(sb['cmdlog']) < 300: sb['cmdlog'].append(f'{where()} DMA out {port:02X},{v:02X}')
def dma_in(port):
    if port <= 7:
        ch = port >> 1
        val = (dma[ch]['addr'] + dma_pos(ch)) & 0xFFFF if not port & 1 else (dma[ch]['cnt'] - dma_pos(ch)) & 0xFFFF
        b = (val >> 8) if dstate['ff'] else (val & 0xFF); dstate['ff'] ^= 1; return b
    if port == 0x08: return 0
    return 0xFF
def sb_in(off):
    if off == 0xA: return sb['outq'].pop(0) if sb['outq'] else 0xAA
    if off == 0xE: sb['irq_req'] = False; return 0x80 if sb['outq'] else 0x00
    if off == 0xC: return 0x00
    if off == 0x5: return sb['mix'].get(sb['mixidx'], 0)
    return 0xFF
def sb_start(length, auto, hs):
    stereo = bool(sb['mix'].get(0x0E, 0) & 2)
    rate = 1e6 / (256 - sb['tc']) if sb['tc'] < 256 else 22050
    sb['run'] = {'t0': vt(), 'bps': rate, 'len': length, 'auto': auto, 'next': vt() + length / rate, 'stereo': stereo,
                 'rate': rate / (2 if stereo else 1)}
    L(f'  SB start len={length} auto={auto} hs={hs} tc={sb["tc"]} byte-rate={rate:.0f} stereo={stereo} DMA{SB_DMA} '
      f'addr={dma[SB_DMA]["page"]:02X}:{dma[SB_DMA]["addr"]:04X} cnt={dma[SB_DMA]["cnt"]} mode={dma[SB_DMA]["mode"]:02X} at {where()}')
def sb_out(off, v):
    if off == 0x6:
        if v & 1: sb['rst'] = 1
        elif sb['rst']: sb['rst'] = 0; sb['outq'] = [0xAA]; sb['run'] = None; sb['cmd'] = None
        return
    if off == 0x4: sb['mixidx'] = v; return
    if off == 0x5: sb['mix'][sb['mixidx']] = v; L(f'  SB mixer[{sb["mixidx"]:02X}]={v:02X}'); return
    if off != 0xC: return
    if sb['cmd'] is None:
        sb['cmd'] = v; sb['args'] = []
    else:
        sb['args'].append(v)
    c = sb['cmd']; need = DSP_ARGS.get(c, 0)
    if len(sb['args']) < need: return
    a = sb['args']; sb['cmd'] = None
    if len(sb['cmdlog']) < 300: sb['cmdlog'].append(f'{where()} DSP {c:02X} {" ".join("%02X" % x for x in a)}')
    if c == 0xE1: sb['outq'] += [3, 2]
    elif c == 0xE0: sb['outq'].append(~a[0] & 0xFF)
    elif c == 0xE3: sb['outq'] += list(b'COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.\0')
    elif c == 0xE8: sb['outq'].append(0)
    elif c == 0xD8: sb['outq'].append(0xFF if sb['spk'] else 0)
    elif c == 0xD1: sb['spk'] = 1
    elif c == 0xD3: sb['spk'] = 0
    elif c == 0x40: sb['tc'] = a[0]
    elif c == 0x48: sb['blk'] = a[0] | a[1] << 8
    elif c == 0x14: sb_start((a[0] | a[1] << 8) + 1, False, False)
    elif c == 0x1C: sb_start(sb['blk'] + 1, True, False)
    elif c == 0x91: sb_start(sb['blk'] + 1, False, True)
    elif c == 0x90: sb_start(sb['blk'] + 1, True, True)
    elif c in (0xD0, 0xDA): sb['run'] = None
    elif c == 0xF2: sb['run'] = {'t0': vt(), 'bps': 1, 'len': 0, 'auto': False, 'next': vt() + 1e-5, 'stereo': 0, 'rate': 1, 'f2': 1}
def sb_capture():
    r = sb['run']
    if not r or r.get('f2'): return
    d = dma[SB_DMA]; base = (d['page'] << 16) | d['addr']; tot = d['cnt'] + 1
    done = min(int((vt() - r['t0']) * r['bps']), r['len'])
    cap = r.setdefault('cap', 0)
    if done <= cap or len(sb['wav']) > 90 * 44100: return
    k = cap
    while k < done:
        i = k % tot if d['mode'] & 0x10 else min(k, tot - 1)
        n = min(done - k, tot - i)
        sb['wav'] += bytes(uc.mem_read(base + i, n)); k += n
    r['cap'] = done
def sb_tick():
    sb_capture()
    r = sb['run']
    if not r or vt() < r['next']: return
    state['irr'] |= 1 << SB_IRQ; ints[('IRQ%d' % SB_IRQ, None)] += 1
    sb['blocks'] += 1
    if r['auto']: r['next'] += r['len'] / r['bps']; r['t0'] = vt(); r['cap'] = 0
    else: sb['run'] = None
state['irr'] = 0; state['isr'] = 0
uc.hook_add(UC_HOOK_INTR, hook_intr)
uc.hook_add(UC_HOOK_CODE, hook_stub, begin=STUBSEG*16, end=STUBSEG*16 + 0xFF)
uc.hook_add(UC_HOOK_INSN, io_in, None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, io_out, None, 1, 0, UC_X86_INS_OUT)

# ---------------- HLE service handlers ----------------
def setcf(f, v):
    return (f | CF) if v else (f & ~CF)
def setzf(f, v):
    return (f | ZF) if v else (f & ~ZF)

def video_scroll(up, n, top, left, bot, right, attr):
    cols = 80; base = 0xB8000
    rows = bot - top + 1
    if n == 0 or n > rows: n = rows
    for r in range(rows):
        dst = top + r if up else bot - r
        src = dst + n if up else dst - n
        for c in range(left, right+1):
            if (up and src <= bot) or (not up and src >= top):
                ww(base + (dst*cols + c)*2, rw(base + (src*cols + c)*2))
            else:
                ww(base + (dst*cols + c)*2, (attr << 8) | 0x20)

def int10(f):
    ax = R(UC_X86_REG_AX); ah, al = ax >> 8, ax & 0xFF; bx = R(UC_X86_REG_BX)
    if ah == 0x00:
        wb(bda+0x49, al & 0x7F); wb(bda+0x84, 24)
        if not al & 0x80:
            for k in range(0, 80*50): ww(0xB8000 + k*2, 0x0720)
    elif ah == 0x01: ww(bda+0x60, R(UC_X86_REG_CX))
    elif ah == 0x02: ww(bda+0x50, R(UC_X86_REG_DX))
    elif ah == 0x03: W(UC_X86_REG_DX, rw(bda+0x50)); W(UC_X86_REG_CX, rw(bda+0x60))
    elif ah == 0x05: pass
    elif ah in (0x06, 0x07):
        cx, dx = R(UC_X86_REG_CX), R(UC_X86_REG_DX)
        video_scroll(ah == 6, al, cx >> 8, cx & 0xFF, dx >> 8, dx & 0xFF, bx >> 8)
    elif ah == 0x08:
        pos = rw(bda+0x50); W(UC_X86_REG_AX, rw(0xB8000 + ((pos >> 8)*80 + (pos & 0xFF))*2))
    elif ah in (0x09, 0x0A):
        pos = rw(bda+0x50); a = 0xB8000 + ((pos >> 8)*80 + (pos & 0xFF))*2
        for k in range(R(UC_X86_REG_CX)):
            wb(a + k*2, al)
            if ah == 9: wb(a + k*2 + 1, bx & 0xFF)
    elif ah == 0x0E:
        tty_out(al)
    elif ah == 0x0F: W(UC_X86_REG_AX, (80 << 8) | rb(bda+0x49)); W(UC_X86_REG_BX, bx & 0xFF)
    elif ah == 0x10:
        if al == 0x1A: pass
        pass
    elif ah == 0x11:
        if al in (0x12, 0x02): wb(bda+0x84, 49); ww(bda+0x85, 8)
        elif al in (0x14, 0x04, 0x11, 0x01): wb(bda+0x84, 24); ww(bda+0x85, 16)
        elif al == 0x30: W(UC_X86_REG_CX, rw(bda+0x85)); W(UC_X86_REG_DX, rb(bda+0x84))
    elif ah == 0x12:
        if (bx & 0xFF) == 0x10: W(UC_X86_REG_BX, 0x0003); W(UC_X86_REG_CX, 0x0009)
    elif ah == 0x1A:
        if al == 0: W(UC_X86_REG_AX, 0x001A); W(UC_X86_REG_BX, 0x0008)
    else:
        return f, False
    return f, True

def tty_out(ch):
    pos = rw(bda+0x50); r, c = pos >> 8, pos & 0xFF; rows = rb(bda+0x84) + 1
    if ch == 0x0D: c = 0
    elif ch == 0x0A: r += 1
    elif ch == 0x08: c = max(0, c-1)
    elif ch == 0x07: pass
    else:
        wb(0xB8000 + (r*80 + c)*2, ch); c += 1
        if c >= 80: c = 0; r += 1
    if r >= rows:
        video_scroll(True, 1, 0, 0, rows-1, 79, 0x07); r = rows-1
    ww(bda+0x50, (r << 8) | c)

def con_write(bs):
    screen_out.append(bs)
    for ch in bs: tty_out(ch)

SC = {}
for row, base in (('1234567890-=', 2), ('qwertyuiop[]', 0x10), ("asdfghjkl;'`", 0x1E), ('\\zxcvbnm,./', 0x2B)):
    for i, ch in enumerate(row): SC[ch] = base + i
SHIFTED = dict(zip('!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:"~|ZXCVBNM<>?', '1234567890-=qwertyuiop[]asdfghjkl;\'`\\zxcvbnm,./'))
TOK = {'ESC': 0x011B, 'ENTER': 0x1C0D, 'TAB': 0x0F09, 'BS': 0x0E08, 'UP': 0x4800, 'DOWN': 0x5000, 'LEFT': 0x4B00,
       'RIGHT': 0x4D00, 'PGUP': 0x4900, 'PGDN': 0x5100, 'HOME': 0x4700, 'END': 0x4F00, 'INS': 0x5200, 'DEL': 0x5300}
for i in range(10): TOK[f'F{i+1}'] = (0x3B + i) << 8
TOK['F11'] = 0x8500; TOK['F12'] = 0x8600
def parse_keys(s):
    out = []; i = 0
    while i < len(s):
        if s[i] == '{':
            j = s.index('}', i); t = s[i+1:j]; i = j + 1
            if t.startswith('W'): out += [None] * int(t[1:] or 300); continue
            if t.startswith('ALT-'): out.append(SC[t[4:].lower()] << 8); continue
            if t.startswith('CTRL-'): c = t[5:].lower(); out.append((SC[c] << 8) | (ord(c) - 96)); continue
            out.append(TOK[t]); continue
        ch = s[i]; i += 1
        if ch == ' ': out.append(0x3920); continue
        base = SHIFTED.get(ch, ch)
        out.append((SC.get(base, 0) << 8) | ord(ch))
    return out
state['keys'] = parse_keys(A.keys)
def next_key():
    while state['keys'] and state['keys'][0] is None: state['keys'].pop(0)
    if state['keys']:
        k = state['keys'].pop(0); L(f'  KEY {k:04X} delivered n={state["n"]} at {where()}'); return k
    return None
def peek_key():
    if state['keys'] and state['keys'][0] is None:
        state['keys'].pop(0); return None
    return state['keys'][0] if state['keys'] else None

def int16(f):
    ah = R(UC_X86_REG_AX) >> 8
    if ah in (0x00, 0x10):
        k = next_key()
        if k is None: state['stop'] = 'blocking INT16 read with no scripted keys'; return f, True
        W(UC_X86_REG_AX, k); return f, True
    if ah in (0x01, 0x11):
        k = peek_key()
        if k is not None:
            W(UC_X86_REG_AX, k); return setzf(f, 0), True
        state['idle'] += 1
        return setzf(f, 1), True
    if ah in (0x02, 0x12): W(UC_X86_REG_AX, 0); return f, True
    return f, False

def int21(f):
    global cwd
    ax = R(UC_X86_REG_AX); ah, al = ax >> 8, ax & 0xFF
    bx, cx, dx = R(UC_X86_REG_BX), R(UC_X86_REG_CX), R(UC_X86_REG_DX)
    ds, es = R(UC_X86_REG_DS), R(UC_X86_REG_ES)
    def ok(**regs):
        for k, v in regs.items(): W(getattr(sys.modules[__name__], 'UC_X86_REG_' + k.upper()), v)
        return setcf(f, 0), True
    def err(code):
        W(UC_X86_REG_AX, code); return setcf(f, 1), True
    if ah == 0x02: con_write(bytes([dx & 0xFF])); return f, True
    if ah == 0x06:
        if (dx & 0xFF) != 0xFF: con_write(bytes([dx & 0xFF])); return f, True
        k = next_key()
        if k is None: W(UC_X86_REG_AX, ax & 0xFF00); return setzf(f, 1), True
        W(UC_X86_REG_AX, (ax & 0xFF00) | (k & 0xFF)); return setzf(f, 0), True
    if ah in (0x07, 0x08, 0x01):
        k = next_key()
        if k is None: state['stop'] = f'blocking DOS read AH={ah:02X}'; return f, True
        W(UC_X86_REG_AX, (ax & 0xFF00) | (k & 0xFF)); return f, True
    if ah == 0x09:
        a = ds*16 + dx; s = bytearray()
        while rb(a) != 0x24 and len(s) < 4096: s.append(rb(a)); a += 1
        con_write(bytes(s)); return f, True
    if ah == 0x0B: W(UC_X86_REG_AX, (ax & 0xFF00) | (0xFF if state['keys'] else 0)); return f, True
    if ah == 0x0E: W(UC_X86_REG_AX, (ax & 0xFF00) | 3); return f, True
    if ah == 0x19: W(UC_X86_REG_AX, (ax & 0xFF00) | 2); return f, True
    if ah == 0x1A: dta[0], dta[1] = ds, dx; return f, True
    if ah == 0x25: ww((al)*4, dx); ww(al*4+2, ds); return f, True
    if ah == 0x2A:
        lt = time.localtime(); W(UC_X86_REG_CX, lt.tm_year); W(UC_X86_REG_DX, (lt.tm_mon << 8) | lt.tm_mday)
        W(UC_X86_REG_AX, (ax & 0xFF00) | ((lt.tm_wday+1) % 7)); return f, True
    if ah == 0x2C:
        lt = time.localtime(); W(UC_X86_REG_CX, (lt.tm_hour << 8) | lt.tm_min); W(UC_X86_REG_DX, lt.tm_sec << 8); return f, True
    if ah == 0x2F: W(UC_X86_REG_ES, dta[0]); W(UC_X86_REG_BX, dta[1]); return f, True
    if ah == 0x30: W(UC_X86_REG_AX, 0x0005); W(UC_X86_REG_BX, 0); W(UC_X86_REG_CX, 0); return f, True
    if ah == 0x33:
        if al == 0: W(UC_X86_REG_DX, dx & 0xFF00)
        return f, True
    if ah == 0x35: W(UC_X86_REG_BX, rw(al*4)); W(UC_X86_REG_ES, rw(al*4+2)); return f, True
    if ah == 0x36:
        W(UC_X86_REG_AX, 8); W(UC_X86_REG_BX, 0x7FFF); W(UC_X86_REG_CX, 512); W(UC_X86_REG_DX, 0xFFFF); return f, True
    if ah == 0x3B:
        p = asciiz(ds*16+dx); hp = host_path(p)
        if not os.path.isdir(hp): return err(3)
        cwd = os.path.relpath(hp, ROOT).replace('/', '\\'); cwd = '' if cwd == '.' else cwd
        return ok()
    if ah in (0x3C, 0x5B):
        p = asciiz(ds*16+dx); hp = host_path(p)
        try: fh = open(hp, 'w+b')
        except OSError: return err(3)
        h = next(i for i in range(5, 255) if i not in handles); handles[h] = fh
        L(f'  CREATE {p!r} -> {h}'); return ok(ax=h)
    if ah == 0x3D:
        p = asciiz(ds*16+dx); hp = host_path(p)
        if p.upper() in ('EMMXXXX0', '\\EMMXXXX0'): L(f'  OPEN {p!r} (EMS probe) -> not found'); return err(2)
        if not os.path.isfile(hp): L(f'  OPEN {p!r} -> not found ({hp})'); return err(2)
        fh = open(hp, 'r+b' if (al & 3) else 'rb')
        h = next(i for i in range(5, 255) if i not in handles); handles[h] = fh
        L(f'  OPEN {p!r} mode {al:02X} -> {h}'); return ok(ax=h)
    if ah == 0x3E:
        fh = handles.pop(bx, None)
        if hasattr(fh, 'close'): fh.close()
        return ok()
    if ah == 0x3F:
        fh = handles.get(bx)
        if fh in ('stdin',):
            state['stop'] = 'read stdin'; return ok(ax=0)
        if not hasattr(fh, 'read'): return err(6)
        d = fh.read(cx); uc.mem_write(ds*16+dx, d); return ok(ax=len(d))
    if ah == 0x40:
        fh = handles.get(bx); d = bytes(uc.mem_read(ds*16+dx, cx))
        if fh in ('stdout', 'stderr'): con_write(d); return ok(ax=cx)
        if not hasattr(fh, 'write'): return err(6)
        if cx == 0: fh.truncate(); return ok(ax=0)
        fh.write(d); return ok(ax=cx)
    if ah == 0x41:
        p = asciiz(ds*16+dx); hp = host_path(p)
        try: os.remove(hp); return ok()
        except OSError: return err(2)
    if ah == 0x42:
        fh = handles.get(bx)
        if not hasattr(fh, 'seek'): return err(6)
        off = struct.unpack('<i', struct.pack('<I', (cx << 16) | dx))[0]
        pos = fh.seek(off, al); return ok(ax=pos & 0xFFFF, dx=pos >> 16)
    if ah == 0x43:
        p = asciiz(ds*16+dx); hp = host_path(p)
        if not os.path.exists(hp): return err(2)
        return ok(cx=0x10 if os.path.isdir(hp) else 0x20)
    if ah == 0x44:
        if al == 0x00:
            fh = handles.get(bx)
            if fh is None: return err(6)
            return ok(dx=0x80D3 if isinstance(fh, str) else 0x0002)
        if al == 0x01: return ok()
        if al == 0x06: W(UC_X86_REG_AX, 0xFF); return setcf(f, 0), True
        if al == 0x07: W(UC_X86_REG_AX, 0xFF); return setcf(f, 0), True
        if al == 0x08: return ok(ax=1)
        if al == 0x09: return ok(dx=0)
        return err(1)
    if ah == 0x47:
        uc.mem_write(ds*16 + R(UC_X86_REG_SI), ('ST3' + ('\\' + cwd if cwd else '')).upper().encode() + b'\0'); return ok()
    if ah == 0x48:
        seg, mx = dos_alloc(bx)
        L(f'  ALLOC {bx:04X} paras -> {seg and hex(seg)} (max free {mx:04X})')
        if seg is None: W(UC_X86_REG_BX, mx); return err(8)
        return ok(ax=seg)
    if ah == 0x49:
        return ok() if dos_free(es) else err(9)
    if ah == 0x4A:
        good, mx = dos_resize(es, bx)
        L(f'  RESIZE {es:04X} to {bx:04X} -> {"ok" if good else "FAIL max %04X" % mx}')
        if not good: W(UC_X86_REG_BX, mx); return err(8)
        return ok()
    if ah == 0x4B: L('  EXEC requested -> fail'); return err(2)
    if ah == 0x4C: state['stop'] = f'EXIT code {al}'; return f, True
    if ah == 0x4E or ah == 0x4F:
        if ah == 0x4E:
            p = asciiz(ds*16+dx); hp = host_path(p); d, pat = os.path.split(hp)
            try: ents = sorted(os.listdir(d))
            except OSError: return err(3)
            res = [(os.path.join(d, e), e) for e in ents if fnmatch83(e, os.path.basename(p.replace('\\', '/')) or '*.*')
                   and (cx & 0x10 or not os.path.isdir(os.path.join(d, e)))]
            if cx & 0x10 and d != ROOT: res = [(d, '.'), (os.path.dirname(d), '..')] + res
            key = dta[0]*16 + dta[1]; finds[key] = res
            L(f'  n={state["n"]} FINDFIRST {p!r} attr {cx:02X} -> {len(res)}')
        else:
            key = dta[0]*16 + dta[1]; res = finds.get(key, [])
        if not res: return err(18 if ah == 0x4F else 2)
        fill_dta(res.pop(0)); return ok()
    if ah == 0x50: return f, True
    if ah in (0x51, 0x62): W(UC_X86_REG_BX, PSP); return f, True
    if ah == 0x56:
        a = host_path(asciiz(ds*16+dx)); b = host_path(asciiz(es*16+R(UC_X86_REG_DI)))
        try: os.rename(a, b); return ok()
        except OSError: return err(2)
    if ah == 0x57:
        if al == 0:
            fh = handles.get(bx); t, d = dos_time(os.fstat(fh.fileno()).st_mtime) if hasattr(fh, 'fileno') else (0, 0)
            return ok(cx=t, dx=d)
        return ok()
    if ah == 0x58: return ok(ax=0)
    if ah == 0x59: return ok(ax=0)
    return f, False

def int2f(f):
    ax = R(UC_X86_REG_AX)
    if ax == 0x4300: W(UC_X86_REG_AX, ax & 0xFF00); return f, True   # no XMS
    if ax == 0x1600: W(UC_X86_REG_AX, 0); return f, True             # no Windows
    return f, True
def int33(f):
    if R(UC_X86_REG_AX) == 0: W(UC_X86_REG_AX, 0); return f, True    # no mouse
    return f, True
def int1a(f):
    ah = R(UC_X86_REG_AX) >> 8
    if ah == 0: t = rw(bda+0x6C) | rw(bda+0x6E) << 16; W(UC_X86_REG_CX, t >> 16); W(UC_X86_REG_DX, t & 0xFFFF); W(UC_X86_REG_AX, 0)
    return f, True
def int15(f):
    return setcf(f, 1), True
def int08(f):   # BIOS timer
    t = (rw(bda+0x6C) | rw(bda+0x6E) << 16) + 1
    ww(bda+0x6C, t & 0xFFFF); ww(bda+0x6E, t >> 16); state['isr'] &= ~1
    return f, True
def int09(f):   # BIOS keyboard (only if program chains)
    state['isr_kb'] = 0; return f, True
def int67(f):
    return f, False

HANDLERS = {0x10: int10, 0x16: int16, 0x21: int21, 0x2F: int2f, 0x33: int33, 0x1A: int1a, 0x15: int15,
            0x08: int08, 0x09: int09, 0x1C: lambda f: (f, True), 0x23: lambda f: (f, True), 0x24: lambda f: (f, True),
            0x67: int67}

def service_stub(no):
    """We are at F000:no — the vectored BIOS/DOS entry. Do HLE then emulate IRET keeping returned CF/ZF."""
    ax = R(UC_X86_REG_AX)
    key = (no, ax >> 8) if no in (0x10, 0x21, 0x16, 0x67, 0x2F, 0x33, 0x15, 0x1A) else (no, None)
    ints[key] += 1
    sp_ = R(UC_X86_REG_SP); ssb = R(UC_X86_REG_SS)*16
    ret_ip, ret_cs, fl = rw(ssb+sp_), rw(ssb+sp_+2), rw(ssb+sp_+4)
    int_first.setdefault(key, f'{ret_cs-LOAD:04X}:{ret_ip:04X} AX={ax:04X}')
    h = HANDLERS.get(no)
    handled = False
    if h:
        fl, handled = h(fl)
    if not handled:
        L(f'  UNHANDLED INT {no:02X} AX={ax:04X} BX={R(UC_X86_REG_BX):04X} from {ret_cs-LOAD:04X}:{ret_ip:04X}')
    pop16(); pop16(); pop16()
    W(UC_X86_REG_CS, ret_cs); W(UC_X86_REG_IP, ret_ip)
    W(UC_X86_REG_EFLAGS, (fl & 0xFFFF) | 2)

# ---------------- main loop ----------------
W(UC_X86_REG_CS, LOAD+cs); W(UC_X86_REG_IP, ip); W(UC_X86_REG_SS, LOAD+ss); W(UC_X86_REG_SP, sp)
W(UC_X86_REG_DS, PSP); W(UC_X86_REG_ES, PSP); W(UC_X86_REG_EFLAGS, 0x202)
def hook_block(u, a, size, ud): state['n'] += size // 3 + 1
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
_md = Cs(CS_ARCH_X86, CS_MODE_16); _dec = {}; census = collections.Counter(); census_where = {}
def hook_census(u, a, size, ud):
    if not (A.census <= state['n'] < A.census + A.census_len): return
    m = _dec.get(a)
    if m is None:
        code = bytes(u.mem_read(a, size)); ins = next(_md.disasm(code, a, 1), None)
        if ins is None: m = ('??', code.hex())
        else:
            pre = 'o32 ' if code[:1] == b'\x66' or b'\x66' in code[:ins.size][:3] and code[0] in (0x66, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0xf3) else ''
            a32 = 'a32 ' if 0x67 in code[:3] else ''
            m = (pre + a32 + ins.mnemonic, ins.op_str)
        _dec[a] = m
    census[m[0]] += 1; census_where.setdefault(m[0], f'{R(UC_X86_REG_CS)-LOAD:04X}:{R(UC_X86_REG_IP):04X} {m[1]}')
if A.census: uc.hook_add(UC_HOOK_CODE, hook_census)
uc.hook_add(UC_HOOK_BLOCK, hook_block)
CHUNK = 4000
t0 = time.time()
while state['n'] < A.insns and not state['stop']:
    try:
        uc.emu_start(R(UC_X86_REG_CS)*16 + R(UC_X86_REG_IP), 0x200000, count=CHUNK)
    except UcError as e:
        L(f'UC ERROR {e} at {where()}'); state['stop'] = f'UC ERROR {e} at {where()}'; break
    if state['pending_stub'] is not None:
        no = state['pending_stub']; state['pending_stub'] = None
        service_stub(no)
        if state['stop']: break
    period = state['pit_div'] / 1193182.0
    if vt() >= state['next_tick']:
        state['next_tick'] = max(state['next_tick'] + period, vt() - 0.5)
        state['irr'] |= 1
    sb_tick()
    if R(UC_X86_REG_EFLAGS) & 0x200:
        for irq in range(8):
            if state['isr'] & ((2 << irq) - 1): break         # same/higher priority in service
            if state['irr'] & ~state['pic_mask'] & (1 << irq):
                state['irr'] &= ~(1 << irq); state['isr'] |= 1 << irq
                if irq == 0: ints[('IRQ0', None)] += 1
                do_int(0x08 + irq); break
dt = time.time() - t0
open(f'{OUT}/font_plane2.bin', 'wb').write(bytes(FONT))
if sb['wav']:
    import wave
    r = sb.get('lastrun') or {}
    with wave.open(f'{OUT}/sb_{A.tag}.wav', 'wb') as w:
        st_ = bool(sb['mix'].get(0x0E, 0) & 2); w.setnchannels(2 if st_ else 1); w.setsampwidth(1)
        w.setframerate(int(1e6 / (256 - sb['tc']) / (2 if st_ else 1))); w.writeframes(bytes(sb['wav']))
open(f'{OUT}/vga_state.json', 'w').write(_json.dumps({'dac': list(DAC), 'attr': list(ATTR), 'rows': rb(bda+0x84)+1,
     'text': list(uc.mem_read(0xB8000, 80*50*2)), 'cursor': rw(bda+0x50), 'charh': rw(bda+0x85)}))

# ---------------- report ----------------
def screen_text():
    rows = rb(bda+0x84) + 1; out = []
    for r in range(rows):
        line = bytes(rb(0xB8000 + (r*80 + c)*2) for c in range(80)).decode('cp437')
        out.append(line.rstrip())
    return '\n'.join(out)
with open(f'{OUT}/survey_{A.tag}.txt', 'w') as fo:
    print(f'stop: {state["stop"]}  insns~{state["n"]:,}  wall {dt:.1f}s  at {where()}', file=fo)
    print(f'font uploads: {state["font_uploads"]}', file=fo)
    print(f'args={A.args} env={A.env} keys left={len(state["keys"])} idle-polls={state["idle"]}', file=fo)
    print('\n== ports (dir port count first-at)', file=fo)
    for (d, p), c in sorted(ports.items(), key=lambda x: (x[0][1], x[0][0])):
        print(f'  {d:3s} {p:04X} {c:8d}  {port_first[(d, p)]}', file=fo)
    print('\n== services (int ah count first-at)', file=fo)
    for k, c in sorted(ints.items(), key=lambda x: (str(x[0][0]), x[0][1] or 0)):
        print(f'  {k[0] if isinstance(k[0], str) else "INT %02X" % k[0]} {"" if k[1] is None else "AH=%02X" % k[1]:6s} {c:7d}  {int_first.get(k, "")}', file=fo)
    print('\n== hooked vectors (not pointing to BIOS stubs)', file=fo)
    for n in range(256):
        if rw(n*4+2) != STUBSEG: print(f'  INT {n:02X} -> {rw(n*4+2)-LOAD:04X}:{rw(n*4):04X} (load-rel)', file=fo)
    if A.census:
        print('\n== opcode census (mnemonic count example)', file=fo)
        for k, c in census.most_common(): print(f'  {k:16s} {c:9d}  {census_where[k]}', file=fo)
    print('\n== SB/DMA log (first 300)', file=fo); print('\n'.join(sb['cmdlog']), file=fo)
    print('\n== VGA reg writes (first 400)', file=fo); print('\n'.join(state.get('vgalog', [])), file=fo)
    print('\n== console output', file=fo); print(b''.join(screen_out).decode('cp437', 'replace'), file=fo)
    print('\n== screen', file=fo); print(screen_text(), file=fo)
print(open(f'{OUT}/survey_{A.tag}.txt').read()[:6000])
