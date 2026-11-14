/* io.c — port I/O dispatch: 8259 PIC, 8254 PIT ch0, 8237 DMA, Sound Blaster Pro (+ OPL2 FM via opl.c), misc. */
#include "machine.h"
#include <string.h>

#define SB_BASE 0x220
#define SB_IRQ 7
#define SB_DMA 1
#define PIT_HZ 1193182.0

double machine_vtime(Machine *m) { return (double)(m->cpu.icount - m->icount_base) / (m->cfg.mips * 1e6); }

/* ------------------------------------------------------------------ PIT */
static uint16_t pit_counter(Machine *m) {
    double el = machine_vtime(m) - m->pit_t0;
    uint32_t div = m->pit_div ? m->pit_div : 65536;
    uint64_t ticks = (uint64_t)(el * PIT_HZ);
    return (uint16_t)(div - (ticks % div));
}

void timers_update(Machine *m) {
    double vt = machine_vtime(m);
    double period = (m->pit_div ? m->pit_div : 65536) / PIT_HZ;
    if (vt >= m->pit_next) {
        m->irr |= 1;
        m->pit_next += period;
        if (m->pit_next < vt - 0.25) m->pit_next = vt + period;   /* don't burst after stalls */
    }
}

/* ------------------------------------------------------------------ DMA */
/* the 8237 keeps its own current address: it advances only as the DSP takes bytes, survives DSP restarts, and is
 * reloaded from the base registers when the program writes address/count (or at terminal count in auto-init mode) */
static uint32_t dma_pos(Machine *m, int ch) { return m->dma[ch].cur; }

static void dma_out(Machine *m, uint16_t port, uint8_t v) {
    static const int pagemap[16] = {[7] = 0, [3] = 1, [1] = 2, [2] = 3};
    if (port <= 7) {
        int ch = port >> 1;
        uint16_t *r = (port & 1) ? &m->dma[ch].cnt : &m->dma[ch].addr;
        if (!m->dma_ff) *r = (*r & 0xFF00) | v; else *r = (*r & 0x00FF) | (v << 8);
        m->dma[ch].cur = 0;
        m->dma_ff ^= 1;
    } else if (port == 0x0A) m->dma[v & 3].mask = (v >> 2) & 1;
    else if (port == 0x0B) m->dma[v & 3].mode = v;
    else if (port == 0x0C) m->dma_ff = 0;
    else if (port == 0x0D) { m->dma_ff = 0; for (int i = 0; i < 4; i++) m->dma[i].mask = 1; }
    else if (port >= 0x81 && port <= 0x87) {
        int idx = port - 0x80;
        if (idx == 7 || idx == 3 || idx == 1 || idx == 2) m->dma[pagemap[idx]].page = v;
    }
}

static uint8_t dma_in(Machine *m, uint16_t port) {
    if (port <= 7) {
        int ch = port >> 1;
        uint32_t p = dma_pos(m, ch);
        uint16_t val = (port & 1) ? (uint16_t)(m->dma[ch].cnt - p) : (uint16_t)(m->dma[ch].addr + p);
        uint8_t b = m->dma_ff ? (val >> 8) : (val & 0xFF);
        m->dma_ff ^= 1;
        return b;
    }
    if (port == 0x08) return 0;
    return 0xFF;
}

/* ------------------------------------------------------------------ Sound Blaster Pro */
static const signed char dsp_nargs[256] = {
    [0x10] = 1, [0x14] = 2, [0x16] = 2, [0x17] = 2, [0x1C] = 0, [0x1F] = 0, [0x40] = 1, [0x41] = 2, [0x42] = 2,
    [0x48] = 2, [0x74] = 2, [0x75] = 2, [0x76] = 2, [0x77] = 2, [0x80] = 2, [0xE0] = 1, [0xE2] = 1, [0xE4] = 1,
    [0x24] = 2,
};

static void sb_outq(Machine *m, uint8_t v) { if (m->sb.outn < 64) m->sb.outq[m->sb.outn++] = v; }

static void sb_start(Machine *m, uint32_t len, int autoinit, int hs) {
    m->sb.running = 1; m->sb.autoinit = autoinit; m->sb.highspeed = hs;
    m->sb.len = len; m->sb.consumed = 0; m->sb.t0 = machine_vtime(m);
    m->sb.bps = m->sb.tc < 256 ? 1e6 / (256 - m->sb.tc) : 22050;
    LOGF(m, "SB start len=%u auto=%d hs=%d tc=%u bps=%.0f stereo=%d dma=%02X:%04X cnt=%u mode=%02X\n",
         len, autoinit, hs, m->sb.tc, m->sb.bps, (m->sb.mix[0x0E] >> 1) & 1,
         m->dma[SB_DMA].page, m->dma[SB_DMA].addr, m->dma[SB_DMA].cnt, m->dma[SB_DMA].mode);
}

static void sb_cmd(Machine *m) {
    uint8_t c = m->sb.cmd, *a = m->sb.args;
    switch (c) {
    case 0xE1: sb_outq(m, 3); sb_outq(m, 2); break;
    case 0xE0: sb_outq(m, ~a[0]); break;
    case 0xE3: { const char *s = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992."; while (*s) sb_outq(m, *s++); sb_outq(m, 0); break; }
    case 0xE4: m->sb.mix[0xFF] = a[0]; break;
    case 0xE8: sb_outq(m, m->sb.mix[0xFF]); break;
    case 0xD8: sb_outq(m, m->sb.spk ? 0xFF : 0); break;
    case 0xD1: m->sb.spk = 1; break;
    case 0xD3: m->sb.spk = 0; break;
    case 0x40: m->sb.tc = a[0]; break;
    case 0x48: m->sb.blk = a[0] | (a[1] << 8); break;
    case 0x14: sb_start(m, (uint32_t)(a[0] | (a[1] << 8)) + 1, 0, 0); break;
    case 0x1C: sb_start(m, (uint32_t)m->sb.blk + 1, 1, 0); break;
    case 0x91: sb_start(m, (uint32_t)m->sb.blk + 1, 0, 1); break;
    case 0x90: sb_start(m, (uint32_t)m->sb.blk + 1, 1, 1); break;
    case 0xD0: case 0xDA: m->sb.running = 0; break;
    case 0xD4: break;
    case 0xF2: m->sb.irq_pending_f2 = 1; m->sb.f2_at = machine_vtime(m) + 10e-6; break;
    case 0x10: break;   /* direct DAC sample: ignore */
    default: LOGF(m, "SB: unhandled DSP cmd %02X\n", c); break;
    }
}

static uint8_t sb_in(Machine *m, int off) {
    switch (off) {
    case 0x0: case 0x2: case 0x8: return opl_read_status(&m->opl, machine_vtime(m));
    case 0xA: {
        if (!m->sb.outn) return 0xAA;
        uint8_t v = m->sb.outq[0];
        memmove(m->sb.outq, m->sb.outq + 1, --m->sb.outn);
        return v; }
    case 0xE: return m->sb.outn ? 0x80 : 0x00;   /* also acks 8-bit IRQ */
    case 0xC: return 0x00;                       /* write buffer ready */
    case 0x5: return m->sb.mix[m->sb.mixidx];
    }
    return 0xFF;
}

static void sb_out(Machine *m, int off, uint8_t v) {
    switch (off) {
    case 0x0: case 0x2: case 0x8: opl_write_addr(&m->opl, v); return;
    case 0x1: case 0x3: case 0x9: mixer_update(m); opl_write_data(&m->opl, v, machine_vtime(m)); return;
    case 0x6:
        if (v & 1) m->sb.rst = 1;
        else if (m->sb.rst) { m->sb.rst = 0; m->sb.outn = 0; sb_outq(m, 0xAA); m->sb.running = 0; m->sb.cmd = -1; }
        return;
    case 0x4: m->sb.mixidx = v; return;
    case 0x5: m->sb.mix[m->sb.mixidx] = v; return;
    case 0xC:
        if (m->sb.cmd < 0) {
            m->sb.cmd = v; m->sb.nargs = 0; m->sb.need = dsp_nargs[v];
        } else if (m->sb.nargs < 4) m->sb.args[m->sb.nargs++] = v;
        if (m->sb.nargs >= m->sb.need) { sb_cmd(m); m->sb.cmd = -1; }
        return;
    }
}

/* advance the virtual DAC: deliver consumed bytes to the PCM sink, raise IRQ at block end */
void sb_update(Machine *m) {
    double vt = machine_vtime(m);
    if (m->sb.irq_pending_f2 && vt >= m->sb.f2_at) { m->sb.irq_pending_f2 = 0; m->irr |= 1 << SB_IRQ; }
    if (!m->sb.running) return;
    uint64_t target = (uint64_t)((vt - m->sb.t0) * m->sb.bps);
    if (target > m->sb.len) target = m->sb.len;
    if (target > m->sb.consumed) {
        uint32_t base = ((uint32_t)m->dma[SB_DMA].page << 16) | m->dma[SB_DMA].addr;
        uint32_t tot = (uint32_t)m->dma[SB_DMA].cnt + 1;
        int autod = m->dma[SB_DMA].mode & 0x10;
        uint8_t buf[4096];
        while (m->sb.consumed < target) {
            uint32_t i = m->dma[SB_DMA].cur;
            if (i >= tot) { if (autod) i = m->dma[SB_DMA].cur = 0; else i = tot - 1; }   /* single-cycle: stuck at end */
            uint32_t n = (uint32_t)(target - m->sb.consumed);
            if (n > tot - i) n = tot - i;
            if (n > sizeof buf) n = sizeof buf;
            for (uint32_t k = 0; k < n; k++) buf[k] = m->mem[(base + i + k) & 0xFFFFF];
            if (m->pcm_sink) {
                int stereo = (m->sb.mix[0x0E] >> 1) & 1;
                mixer_dac(m, buf, (int)n, m->sb.bps / (stereo ? 2 : 1), stereo);
            }
            m->sb.consumed += n;
            m->dma[SB_DMA].cur = autod ? (i + n) % tot : i + n;   /* single-cycle: reaches tot (count reads FFFFh) */
        }
    }
    if (m->sb.consumed >= m->sb.len) {
        m->irr |= 1 << SB_IRQ;
        if (m->sb.autoinit) { m->sb.t0 += m->sb.len / m->sb.bps; m->sb.consumed = 0; }
        else m->sb.running = 0;
    }
}

/* ------------------------------------------------------------------ dispatch */
uint32_t io_in(void *ud, uint16_t port, int size) {
    Machine *m = ud;
    if (size > 1) {   /* split wide accesses */
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v |= io_in(ud, port + i, 1) << (8 * i);
        return v;
    }
    if (port <= 0x0F || (port >= 0x80 && port <= 0x8F)) return dma_in(m, port);
    if (port == 0x20) return m->isr;
    if (port == 0x21) return m->pic_mask;
    if (port == 0x40) {
        if (!m->pit_latched) { uint16_t c = pit_counter(m); m->pit_latch_lo = c; m->pit_latch_hi = c >> 8; m->pit_latched = 1; m->pit_read_hi = 0; }
        uint8_t v = m->pit_read_hi ? m->pit_latch_hi : m->pit_latch_lo;
        if (m->pit_read_hi) m->pit_latched = 0;
        m->pit_read_hi ^= 1;
        return v;
    }
    if (port == 0x41 || port == 0x42) return 0;
    if (port == 0x60) return 0;
    if (port == 0x61) { m->port61 ^= 0x10; return m->port61; }   /* refresh toggle bit 4 */
    if (port == 0x64) return 0x14;
    if (port >= SB_BASE && port <= SB_BASE + 0xF) return sb_in(m, port - SB_BASE);
    if (port == 0x388) return opl_read_status(&m->opl, machine_vtime(m));
    if (port >= 0x3C0 && port <= 0x3DF) return vga_in(m, port);
    return 0xFF;
}

void io_out(void *ud, uint16_t port, int size, uint32_t val) {
    Machine *m = ud;
    if (size > 1) {
        for (int i = 0; i < size; i++) io_out(ud, port + i, 1, (val >> (8 * i)) & 0xFF);
        return;
    }
    uint8_t v = val & 0xFF;
    if (port <= 0x0F || (port >= 0x80 && port <= 0x8F)) { dma_out(m, port, v); return; }
    if (port == 0x20) {
        if ((v & 0xE0) == 0x20) { for (int b = 0; b < 8; b++) if (m->isr & (1 << b)) { m->isr &= ~(1 << b); break; } }
        return;
    }
    if (port == 0x21) { m->pic_mask = v; return; }
    if (port == 0x43) {
        if ((v & 0xC0) == 0x00 && (v & 0x30) == 0) {   /* latch ch0 */
            uint16_t c = pit_counter(m); m->pit_latch_lo = c; m->pit_latch_hi = c >> 8; m->pit_latched = 1; m->pit_read_hi = 0;
        } else if ((v & 0xC0) == 0) m->pit_lohi = 0;
        return;
    }
    if (port == 0x40) {
        if (m->pit_lohi == 0) { m->pit_lo = v; m->pit_lohi = 1; }
        else {
            uint32_t old = m->pit_div;
            m->pit_div = m->pit_lo | (v << 8); m->pit_lohi = 0;
            m->pit_t0 = machine_vtime(m);
            m->pit_next = m->pit_t0 + (m->pit_div ? m->pit_div : 65536) / PIT_HZ;
            if (old != m->pit_div) LOGF(m, "PIT ch0 divisor %u (%.2f Hz)\n", m->pit_div, PIT_HZ / (m->pit_div ? m->pit_div : 65536));
        }
        return;
    }
    if (port == 0x61) { m->port61 = v; return; }
    if (port >= SB_BASE && port <= SB_BASE + 0xF) { sb_out(m, port - SB_BASE, v); return; }
    if (port == 0x388) { opl_write_addr(&m->opl, v); return; }
    if (port == 0x389) { mixer_update(m); opl_write_data(&m->opl, v, machine_vtime(m)); return; }
    if (port >= 0x3C0 && port <= 0x3DF) { vga_out(m, port, v); return; }
}
