/* render.c — VGA text screen -> terminal (UTF-8 + 24-bit ANSI color), with ST3 glyph mapping and frame diffing. */
#include "render.h"
#include <string.h>
#include <stdio.h>

/* CP437 -> Unicode */
static const uint16_t cp437[256] = {
    0x0020,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,
    0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x2302,
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

/* ST3's own UI glyphs in font block 0 (identified from bitmaps; see notes/JOURNAL.md m3e) */
static uint32_t st3_hand(int code) {
    switch (code) {
    case 0xE0: return 0x259B;   /* top+left bevel corner      ▛ */
    case 0xE1: return 0x259F;   /* right+bottom bevel corner  ▟ */
    case 0xE2: return 0x25E4;   /* upper-left triangle        ◤ */
    case 0xE3: return 0x25E3;   /* lower-left triangle        ◣ */
    case 0xE4: return 0x25AE;   /* VU segment                 ▮ */
    case 0xE6: return 0x2503;   /* centered thick bar         ┃ */
    case 0xE7: return 0x25B8;   /* small right triangle       ▸ */
    case 0xE8: return 0x2596;   /* lower-left dot             ▖ */
    case 0xE9: return 0x2598;   /* upper-left dot             ▘ */
    case 0xEA: return 0x259D;   /* upper-right dot            ▝ */
    case 0xEB: return 0x2597;   /* lower-right dot            ▗ */
    case 0xEC: return 0x2582;   /* bottom edge                ▂ */
    case 0xED: return 0x2594;   /* top edge                   ▔ */
    case 0xEE: return 0x2595;   /* right edge                 ▕ */
    case 0xEF: return 0x258E;   /* left edge                  ▎ */
    case 0xF4: return 0x25B4;   /* bar + up triangle          ▴ */
    case 0xF9: return 0x00B7;   /* center dot                 · */
    case 0xFB: return 0x2611;   /* checked box                ☑ */
    case 0xFC: return 0x2610;   /* empty box                  ☐ */
    case 0xFD: return 0x2504;   /* dotted rule                ┄ */
    }
    return 0;
}

/* automatic bitmap -> block element (quadrants) for glyphs with no mapping (logo tiles, runtime-drawn glyphs) */
static uint32_t quad_char(const uint8_t *g, int h) {
    static const uint32_t Q[16] = {0x20, 0x2598, 0x259D, 0x2580, 0x2596, 0x258C, 0x259E, 0x259B,
                                   0x2597, 0x259A, 0x2590, 0x259C, 0x2584, 0x2599, 0x259F, 0x2588};
    int cnt[4] = {0}, tot[4] = {0};
    for (int y = 0; y < h; y++) for (int x = 0; x < 8; x++) {
        int q = (y >= h / 2 ? 2 : 0) + (x >= 4 ? 1 : 0);
        tot[q]++; if (g[y] & (0x80 >> x)) cnt[q]++;
    }
    int bits = 0;
    for (int q = 0; q < 4; q++) if (cnt[q] * 3 >= tot[q]) bits |= 1 << q;   /* >= 1/3 filled */
    return Q[bits];
}

static int glyph_blank(const uint8_t *g, int h) { for (int y = 0; y < h; y++) if (g[y]) return 0; return 1; }

void render_build_map(Render *r, const uint8_t *font, int charh, int mode) {
    for (int blk = 0; blk < 2; blk++)
        for (int c = 0; c < 256; c++) {
            const uint8_t *g = font + blk * 0x4000 + c * 32;
            uint32_t u;
            if (glyph_blank(g, charh)) u = 0x20;
            else if (blk == 1 && !memcmp(g, font + c * 32, charh)) u = r->map[0][c];   /* same bitmap as text font */
            else if (blk == 0 && st3_hand(c) && mode != 1) u = st3_hand(c);
            else if (blk == 0 && mode != 1) u = cp437[c];
            else u = quad_char(g, charh);
            r->map[blk][c] = u;
        }
}

static int utf8(uint32_t u, char *o) {
    if (u < 0x80) { o[0] = u; return 1; }
    if (u < 0x800) { o[0] = 0xC0 | (u >> 6); o[1] = 0x80 | (u & 0x3F); return 2; }
    if (u < 0x10000) { o[0] = 0xE0 | (u >> 12); o[1] = 0x80 | ((u >> 6) & 0x3F); o[2] = 0x80 | (u & 0x3F); return 3; }
    o[0] = 0xF0 | (u >> 18); o[1] = 0x80 | ((u >> 12) & 0x3F); o[2] = 0x80 | ((u >> 6) & 0x3F); o[3] = 0x80 | (u & 0x3F); return 4;
}

static uint32_t attr_rgb(const uint8_t *dac, const uint8_t *attr, int ix) {
    int p = attr[ix & 15] & 0x3F;
    if (attr[0x10] & 0x80) p = (p & 0x0F) | ((attr[0x14] & 3) << 4);
    p |= (attr[0x14] & 0x0C) << 4;
    int rr = dac[p * 3] * 255 / 63, gg = dac[p * 3 + 1] * 255 / 63, bb = dac[p * 3 + 2] * 255 / 63;
    return (rr << 16) | (gg << 8) | bb;
}

void render_init(Render *r) { memset(r, 0, sizeof *r); r->force = 1; }

/* produce terminal output for the current screen into r->out (returns length) */
int render_frame(Render *r, const uint8_t *vram, int rows, const uint8_t *dac, const uint8_t *attr, uint8_t seq3,
                 int term_rows, int term_cols, int y0, int x0) {
    char *o = r->out; int n = 0;
    int blink = attr[0x10] & 0x08;
    /* character map select (SEQ3): A = attr bit3 set, B = clear. block index as plane offset/16K for maps 0,1 */
    int mapA = ((seq3 >> 2) & 3) | ((seq3 >> 3) & 4), mapB = (seq3 & 3) | ((seq3 >> 2) & 4);
    uint32_t last_fg = 0xFFFFFFFF, last_bg = 0xFFFFFFFF;
    int cur_r = -1, cur_c = -1;
    if (rows > 50) rows = 50;
    for (int y = 0; y < rows && y + y0 < term_rows; y++) {
        for (int x = 0; x < 80 && x + x0 < term_cols; x++) {
            uint8_t ch = vram[(y * 80 + x) * 2], at = vram[(y * 80 + x) * 2 + 1];
            uint32_t fg = attr_rgb(dac, attr, at & 15);
            uint32_t bg = attr_rgb(dac, attr, (at >> 4) & (blink ? 7 : 15));
            int map = (at & 8) ? mapA : mapB;
            int blk = map == 0 ? 0 : 1;   /* we capture maps 0 and 1 (plane offsets 0 and 16K) */
            uint32_t u = r->map[blk][ch];
            Cell cell = {u, fg, bg};
            Cell *prev = &r->prev[y][x];
            if (!r->force && prev->u == cell.u && prev->fg == cell.fg && prev->bg == cell.bg) continue;
            *prev = cell;
            if (n > (int)sizeof r->out - 64) break;
            if (cur_r != y || cur_c != x) n += sprintf(o + n, "\x1b[%d;%dH", y + y0 + 1, x + x0 + 1);
            if (fg != last_fg) { n += sprintf(o + n, "\x1b[38;2;%u;%u;%um", fg >> 16, (fg >> 8) & 255, fg & 255); last_fg = fg; }
            if (bg != last_bg) { n += sprintf(o + n, "\x1b[48;2;%u;%u;%um", bg >> 16, (bg >> 8) & 255, bg & 255); last_bg = bg; }
            n += utf8(u, o + n);
            cur_r = y; cur_c = x + 1;
        }
    }
    r->force = 0;
    return n;
}
