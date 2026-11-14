/* vga.c — VGA register model for text mode: attribute controller, DAC, sequencer, graphics controller, CRTC,
 * input status (70 Hz retrace in virtual time), and plane-2 font capture via the B800 window. */
#include "machine.h"
#include <string.h>

#define VGA_LINE_HZ 31469.0
#define VGA_LINES 449

void vga_init(Machine *m) {
    static const uint8_t ega[16] = {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};
    for (int i = 0; i < 64; i++) {
        int r = ((i >> 2) & 1) * 2 + ((i >> 5) & 1), g = ((i >> 1) & 1) * 2 + ((i >> 4) & 1), b = (i & 1) * 2 + ((i >> 3) & 1);
        m->dac[i * 3] = r * 21; m->dac[i * 3 + 1] = g * 21; m->dac[i * 3 + 2] = b * 21;
    }
    memcpy(m->attr, ega, 16);
    m->attr[0x10] = 0x0C; m->attr[0x12] = 0x0F; m->attr[0x13] = 0x08; m->attr[0x14] = 0;
    /* standard VGA mode 3 (80x25, 9x16 cells, 720x400 @ 70 Hz) register values, as left by the BIOS */
    static const uint8_t crtc3[25] = {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00,
                                      0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF};
    static const uint8_t seq3[5] = {0x03, 0x00, 0x03, 0x00, 0x02};
    static const uint8_t gc3[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF};
    memcpy(m->crtc, crtc3, sizeof crtc3); memcpy(m->seq, seq3, sizeof seq3); memcpy(m->gc, gc3, sizeof gc3);
    m->misc_out = 0x67;
}

static uint32_t font_window(Machine *m) {
    switch ((m->gc[6] >> 2) & 3) { case 0: case 1: return 0xA0000; case 2: return 0xB0000; default: return 0xB8000; }
}
static void vga_mapmask(Machine *m, uint8_t mm) {
    if (mm == 0x04 && !m->fontmode) {
        m->font_base = font_window(m);
        memcpy(m->saved_text, m->mem + m->font_base, 0x8000);
        memcpy(m->mem + m->font_base, m->font, 0x8000);
        m->fontmode = 1;
    } else if (mm != 0x04 && m->fontmode) {
        memcpy(m->font, m->mem + m->font_base, 0x8000);
        memcpy(m->mem + m->font_base, m->saved_text, 0x8000);
        m->fontmode = 0;
        m->font_uploads++;
        m->video_dirty = 1;
        if (m->font_uploads <= 8 || m->font_uploads % 500 == 0) LOGF(m, "VGA font upload #%d via %05X\n", m->font_uploads, m->font_base);
    }
}

uint8_t vga_in(Machine *m, uint16_t port) {
    switch (port) {
    case 0x3C1: return m->attr[m->attr_idx & 0x1F];
    case 0x3C5: return m->seq[m->seq_idx & 7];
    case 0x3C7: return 0x00;
    case 0x3C8: return (m->dac_w / 3) & 0xFF;
    case 0x3C9: { uint8_t v = m->dac[m->dac_r % 768]; m->dac_r = (m->dac_r + 1) % 768; return v; }
    case 0x3CC: return m->misc_out;
    case 0x3CF: return m->gc[m->gc_idx & 15];
    case 0x3D4: return m->crtc_idx;
    case 0x3D5: return m->crtc[m->crtc_idx & 31];
    case 0x3DA: {
        m->attr_flip = 0;
        double t = machine_vtime(m) * VGA_LINE_HZ;
        uint64_t line_total = (uint64_t)t;
        double frac = t - (double)line_total;
        int line = (int)(line_total % VGA_LINES);
        uint8_t v = 0;
        if (line >= 400 || frac > 0.8) v |= 0x01;
        if (line >= 412 && line < 414) v |= 0x08;
        return v;
    }
    }
    return 0xFF;
}

void vga_out(Machine *m, uint16_t port, uint8_t v) {
    switch (port) {
    case 0x3C0:
        if (!m->attr_flip) m->attr_idx = v & 0x3F;
        else { m->attr[m->attr_idx & 0x1F] = v; m->video_dirty = 1; }
        m->attr_flip ^= 1;
        break;
    case 0x3C2: m->misc_out = v; break;
    case 0x3C4: m->seq_idx = v; break;
    case 0x3C5:
        m->seq[m->seq_idx & 7] = v;
        if ((m->seq_idx & 7) == 2) vga_mapmask(m, v);
        if ((m->seq_idx & 7) == 3) m->video_dirty = 1;
        break;
    case 0x3C7: m->dac_r = (v * 3) % 768; break;
    case 0x3C8: m->dac_w = (v * 3) % 768; break;
    case 0x3C9: m->dac[m->dac_w] = v & 0x3F; m->dac_w = (m->dac_w + 1) % 768; m->video_dirty = 1; break;
    case 0x3CE: m->gc_idx = v; break;
    case 0x3CF: m->gc[m->gc_idx & 15] = v; break;
    case 0x3D4: m->crtc_idx = v; break;
    case 0x3D5: m->crtc[m->crtc_idx & 31] = v; m->video_dirty = 1; break;
    }
}
