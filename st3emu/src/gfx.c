/* gfx.c — show the exact VGA picture (vgafb.c) in a terminal with an inline-graphics protocol:
 *   iterm : iTerm2 inline images (OSC 1337 File=, PNG)  — iTerm2, WezTerm, ChromeOS Terminal (hterm)
 *   kitty : kitty graphics protocol (APC G, PNG, image replaced in place) — kitty, WezTerm, Ghostty, Konsole
 *   sixel : DEC sixel (palette + RLE)                  — xterm -ti vt340, foot, mlterm, WezTerm, Windows Terminal
 * The picture is placed in the largest centered cell box with the monitor's 4:3 shape.  Only changed frames are sent. */
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put(Gfx *g, const void *d, size_t n) {
    if (g->n + n > g->cap) { g->cap = (g->n + n) * 2 + 65536; g->out = realloc(g->out, g->cap); }
    memcpy(g->out + g->n, d, n); g->n += n;
}
static void puts_(Gfx *g, const char *s) { put(g, s, strlen(s)); }
static void putf(Gfx *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void putf(Gfx *g, const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); int k = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    put(g, b, k > 0 ? (size_t)k : 0);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void put_b64(Gfx *g, const uint8_t *d, size_t n) {
    size_t need = (n + 2) / 3 * 4;
    if (g->n + need > g->cap) { g->cap = (g->n + need) * 2 + 65536; g->out = realloc(g->out, g->cap); }
    char *o = g->out + g->n;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *o++ = B64[v >> 18]; *o++ = B64[(v >> 12) & 63];
        *o++ = i + 1 < n ? B64[(v >> 6) & 63] : '='; *o++ = i + 2 < n ? B64[v & 63] : '=';
    }
    g->n += need;
}

/* ---- scaling: nearest neighbour from the w x h picture into an sw x sh image of DAC indices ---- */
static uint8_t *scale(Gfx *g, const VgaFrame *f, int sw, int sh) {
    size_t need = (size_t)sw * sh;
    if (need > g->scap) { g->scap = need; g->sbuf = realloc(g->sbuf, need); }
    int *xs = malloc(sizeof(int) * sw);
    for (int x = 0; x < sw; x++) xs[x] = (int)((long)x * f->w / sw);
    for (int y = 0; y < sh; y++) {
        int sy = (int)((long)y * f->h / sh);
        uint8_t *o = g->sbuf + (size_t)y * sw;
        if (y && sy == (int)((long)(y - 1) * f->h / sh)) { memcpy(o, o - sw, sw); continue; }
        const uint8_t *s = f->pix + sy * f->w;
        for (int x = 0; x < sw; x++) o[x] = s[xs[x]];
    }
    free(xs);
    return g->sbuf;
}

/* ---- PNG (8-bit indexed, filter "Up" so repeated scanlines cost nothing) ---- */
static void chunk(uint8_t **p, size_t *n, size_t *cap, const char *type, const uint8_t *d, uint32_t len) {
    if (*n + len + 12 > *cap) { *cap = (*n + len + 12) * 2; *p = realloc(*p, *cap); }
    uint8_t *o = *p + *n;
    o[0] = len >> 24; o[1] = len >> 16; o[2] = len >> 8; o[3] = len;
    memcpy(o + 4, type, 4); if (len) memcpy(o + 8, d, len);
    uint32_t c = crc32(crc32(0, o + 4, 4), d, len);
    o[8 + len] = c >> 24; o[9 + len] = c >> 16; o[10 + len] = c >> 8; o[11 + len] = c;
    *n += len + 12;
}
static size_t png_encode(Gfx *g, const uint8_t *img, int w, int h, const uint8_t *pal, uint8_t **outp) {
    size_t rawn = (size_t)(w + 1) * h;
    uint8_t *raw = malloc(rawn);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (w + 1);
        const uint8_t *s = img + (size_t)y * w;
        if (y == 0) { r[0] = 0; memcpy(r + 1, s, w); }
        else { r[0] = 2; for (int x = 0; x < w; x++) r[1 + x] = s[x] - s[x - w]; }
    }
    uLongf zn = compressBound(rawn);
    uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, rawn, 1);
    free(raw);
    size_t n = 0, cap = zn + 1024;
    uint8_t *p = malloc(cap);
    memcpy(p, "\x89PNG\r\n\x1a\n", 8); n = 8;
    uint8_t ih[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 3, 0, 0, 0};
    chunk(&p, &n, &cap, "IHDR", ih, 13);
    chunk(&p, &n, &cap, "PLTE", pal, 768);
    chunk(&p, &n, &cap, "IDAT", z, (uint32_t)zn);
    chunk(&p, &n, &cap, "IEND", NULL, 0);
    free(z);
    (void)g;
    *outp = p;
    return n;
}

/* ---- sixel ---- */
static void sixel_encode(Gfx *g, const uint8_t *img, int w, int h, const uint8_t *pal) {
    int used[256] = {0}, map[256], nc = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) used[img[i]] = 1;
    puts_(g, "\x1bP0;1;0q\"1;1;"); putf(g, "%d;%d", w, h);
    for (int i = 0; i < 256; i++) if (used[i]) {
        map[i] = nc;
        putf(g, "#%d;2;%d;%d;%d", nc, pal[i * 3] * 100 / 255, pal[i * 3 + 1] * 100 / 255, pal[i * 3 + 2] * 100 / 255);
        nc++;
    }
    uint8_t *bits = malloc(w);
    for (int y0 = 0; y0 < h; y0 += 6) {
        int first = 1;
        for (int c = 0; c < 256; c++) {
            if (!used[c]) continue;
            int any = 0;
            for (int x = 0; x < w; x++) {
                uint8_t b = 0;
                for (int k = 0; k < 6 && y0 + k < h; k++) if (img[(size_t)(y0 + k) * w + x] == c) b |= 1 << k;
                bits[x] = b; any |= b;
            }
            if (!any) continue;
            if (!first) puts_(g, "$");
            first = 0;
            putf(g, "#%d", map[c]);
            int last = w; while (last > 0 && !bits[last - 1]) last--;
            for (int x = 0; x < last;) {
                int r = 1; while (x + r < last && bits[x + r] == bits[x]) r++;
                char ch = (char)(63 + bits[x]);
                if (r > 3) putf(g, "!%d%c", r, ch); else for (int k = 0; k < r; k++) put(g, &ch, 1);
                x += r;
            }
        }
        puts_(g, "-");
    }
    free(bits);
    puts_(g, "\x1b\\");
}

/* largest cell box with the 4:3 monitor shape (cells are cw x ch pixels; unknown -> assume 1:2) */
static void layout(int rows, int cols, int cw, int chh, int *bx, int *by, int *bw, int *bh) {
    double cell_aspect = (cw > 0 && chh > 0) ? (double)chh / cw : 2.0;   /* height / width */
    int usable_rows = rows - 1;                                          /* never touch the last line (no scroll) */
    int w = cols, h = (int)(w * 3.0 / 4.0 / cell_aspect);
    if (h > usable_rows) { h = usable_rows; w = (int)(h * cell_aspect * 4.0 / 3.0); }
    if (w > cols) w = cols;
    *bw = w; *bh = h; *bx = (cols - w) / 2; *by = (usable_rows - h) / 2;
}

int gfx_parse_mode(const char *s) {
    if (!strcmp(s, "iterm")) return GFX_ITERM;
    if (!strcmp(s, "kitty")) return GFX_KITTY;
    if (!strcmp(s, "sixel")) return GFX_SIXEL;
    if (!strcmp(s, "text")) return GFX_TEXT;
    return -1;
}

void gfx_init(Gfx *g, int mode) { memset(g, 0, sizeof *g); g->mode = mode; g->force = 1; }

size_t gfx_frame(Gfx *g, const VgaFrame *f, int rows, int cols, int xpix, int ypix) {
    g->n = 0;
    int cw = cols ? xpix / cols : 0, chh = rows ? ypix / rows : 0;
    int changed = g->force || f->w != g->last.w || f->h != g->last.h || rows != g->rows || cols != g->cols ||
                  memcmp(f->pal, g->last.pal, 768) || memcmp(f->pix, g->last.pix, (size_t)f->w * f->h);
    if (!changed) return 0;
    if (g->force || rows != g->rows || cols != g->cols) puts_(g, "\x1b[0m\x1b[2J");
    g->force = 0; g->rows = rows; g->cols = cols;
    g->last.w = f->w; g->last.h = f->h;
    memcpy(g->last.pal, f->pal, 768); memcpy(g->last.pix, f->pix, (size_t)f->w * f->h);
    int bx, by, bw, bh;
    layout(rows, cols, cw, chh, &bx, &by, &bw, &bh);
    if (bw < 8 || bh < 4) return g->n;
    putf(g, "\x1b[%d;%dH", by + 1, bx + 1);
    if (g->mode == GFX_SIXEL) {
        /* sixel pixels are screen pixels: scale to the box (needs the cell size; else 2x horizontally, 2.4x vertically) */
        int sw = cw > 0 ? bw * cw : f->w * 2, sh = chh > 0 ? bh * chh : f->h * 12 / 5;
        sixel_encode(g, scale(g, f, sw, sh), sw, sh, f->pal);
    } else {
        /* the terminal scales the image into the cell box; pre-scale by 2 (nearest) so its smoothing stays crisp */
        int sw = f->w * 2, sh = f->h * 2;
        uint8_t *png; size_t pn = png_encode(g, scale(g, f, sw, sh), sw, sh, f->pal, &png);
        if (g->mode == GFX_ITERM) {
            putf(g, "\x1b]1337;File=inline=1;size=%zu;width=%d;height=%d;preserveAspectRatio=0;doNotMoveCursor=1:", pn, bw, bh);
            put_b64(g, png, pn);
            puts_(g, "\a");
        } else {   /* kitty: transmit+display, image id 1 / placement 1 replaced each frame, chunks of 4096 */
            size_t b64n = (pn + 2) / 3 * 4;
            size_t start = g->n;
            put_b64(g, png, pn);
            char *b = malloc(b64n); memcpy(b, g->out + start, b64n); g->n = start;
            for (size_t off = 0; off < b64n; off += 4096) {
                size_t k = b64n - off < 4096 ? b64n - off : 4096;
                if (off == 0) putf(g, "\x1b_Ga=T,f=100,i=1,p=1,q=2,C=1,c=%d,r=%d,m=%d;", bw, bh, off + k < b64n);
                else putf(g, "\x1b_Gm=%d;", off + k < b64n);
                put(g, b + off, k);
                puts_(g, "\x1b\\");
            }
            free(b);
        }
        free(png);
    }
    return g->n;
}
