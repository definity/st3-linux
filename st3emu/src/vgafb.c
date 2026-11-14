/* vgafb.c — exact VGA text-mode raster: turns video memory + font planes + registers into the picture the VGA
 * would send to the monitor (one DAC index per pixel), the way the hardware does it:
 *   geometry from the CRTC (columns, vertical display end, max scan line, double scan, start address, offset),
 *   8/9-dot cells (SEQ1 bit 0) with line-graphics column duplication for C0h-DFh (attr mode bit 2),
 *   character map select (SEQ3) by attribute bit 3, blink (attr mode bit 3) at vsync/32, cursor at vsync/16,
 *   attribute controller palette (color plane enable, P5-4 select, color select) -> DAC index. */
#include "vgafb.h"
#include <string.h>

/* font plane offset of character map n (SEQ3 numbering); the captured window covers plane offsets 0..32K */
static const uint32_t map_off[8] = {0x0000, 0x4000, 0x8000, 0xC000, 0x2000, 0x6000, 0xA000, 0xE000};

static int dac_index(const Machine *m, int idx) {
    const uint8_t *a = m->attr;
    int p = a[idx & a[0x12] & 0x0F];
    if (a[0x10] & 0x80) p = (p & 0x0F) | ((a[0x14] & 3) << 4); else p &= 0x3F;
    return p | ((a[0x14] & 0x0C) << 4);
}

void vgafb_geometry(const Machine *m, int *w, int *h) {
    int cols = m->crtc[1] + 1, dots = (m->seq[1] & 1) ? 8 : 9;
    int vde = (m->crtc[0x12] | ((m->crtc[7] & 0x02) << 7) | ((m->crtc[7] & 0x40) << 3)) + 1;
    *w = cols * dots; *h = vde;
    if (*w > VGAFB_MAXW) *w = VGAFB_MAXW;
    if (*h > VGAFB_MAXH) *h = VGAFB_MAXH;
}

void vgafb_render(const Machine *m, VgaFrame *f, unsigned frame) {
    int w, h; vgafb_geometry(m, &w, &h);
    f->w = w; f->h = h;
    int cols = m->crtc[1] + 1, dots = (m->seq[1] & 1) ? 8 : 9;
    int charh = (m->crtc[9] & 0x1F) + 1, dbl = (m->crtc[9] & 0x80) ? 1 : 0;
    uint32_t start = (m->crtc[0x0C] << 8) | m->crtc[0x0D];
    int pitch = m->crtc[0x13] * 2;                         /* character positions per row */
    int lge = m->attr[0x10] & 0x04, blink_en = m->attr[0x10] & 0x08;
    int blink_on = (frame >> 4) & 1, cursor_on = (frame >> 3) & 1;
    int mapA = ((m->seq[3] >> 2) & 3) | ((m->seq[3] >> 3) & 4), mapB = (m->seq[3] & 3) | ((m->seq[3] >> 2) & 4);
    uint32_t cur = (m->crtc[0x0E] << 8) | m->crtc[0x0F];
    int cur_en = !(m->crtc[0x0A] & 0x20), cs = m->crtc[0x0A] & 0x1F, ce = m->crtc[0x0B] & 0x1F;
    int idx[16]; for (int i = 0; i < 16; i++) idx[i] = dac_index(m, i);
    const uint8_t *vram = m->mem + 0xB8000;
    for (int y = 0; y < h; y++) {
        int sl = y >> dbl, row = sl / charh, line = sl % charh;
        uint8_t *o = f->pix + y * w;
        for (int c = 0; c < cols && c * dots < w; c++) {
            uint32_t pos = (start + row * pitch + c) & 0x3FFF;   /* 16K character positions in the 32K text window */
            uint8_t ch = vram[pos * 2], at = vram[pos * 2 + 1];
            int fg = at & 15, bg = at >> 4;
            if (blink_en) { bg &= 7; if ((at & 0x80) && !blink_on) fg = bg; }
            uint32_t off = map_off[(at & 8) ? mapA : mapB];
            uint8_t bits = off < 0x8000 ? m->font[off + ch * 32 + line] : 0;
            if (cur_en && cursor_on && pos == cur && line >= cs && line <= ce) bits = 0xFF;
            int fgi = idx[fg], bgi = idx[bg];
            for (int x = 0; x < 8; x++) o[c * dots + x] = (bits & (0x80 >> x)) ? fgi : bgi;
            if (dots == 9) o[c * dots + 8] = (lge && ch >= 0xC0 && ch <= 0xDF && (bits & 1)) || (bits == 0xFF && pos == cur) ? fgi : bgi;
        }
    }
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++) { int v = m->dac[i * 3 + k] & 0x3F; f->pal[i * 3 + k] = (uint8_t)((v * 255 + 31) / 63); }   /* 6-bit DAC -> 8-bit, rounded */
}
