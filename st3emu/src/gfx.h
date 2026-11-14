#ifndef ST3_GFX_H
#define ST3_GFX_H
#include "vgafb.h"
#include <stddef.h>
enum { GFX_TEXT = 0, GFX_ITERM, GFX_KITTY, GFX_SIXEL };
typedef struct {
    int mode, force, rows, cols;
    VgaFrame last;               /* last frame sent (change detection) */
    char *out; size_t n, cap;    /* escape-sequence output of the last gfx_frame() */
    uint8_t *sbuf; size_t scap;  /* scaled image */
} Gfx;
int gfx_parse_mode(const char *s);   /* "iterm" | "kitty" | "sixel" | "text"; -1 if unknown */
void gfx_init(Gfx *g, int mode);
/* encode the frame for the terminal (rows x cols cells, xpix x ypix pixels or 0 if unknown); returns bytes in g->out,
 * 0 if nothing changed */
size_t gfx_frame(Gfx *g, const VgaFrame *f, int rows, int cols, int xpix, int ypix);
#endif
