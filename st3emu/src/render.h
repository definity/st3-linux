#ifndef ST3_RENDER_H
#define ST3_RENDER_H
#include <stdint.h>
typedef struct { uint32_t u, fg, bg; } Cell;
typedef struct {
    uint32_t map[2][256];     /* glyph -> Unicode for font blocks 0/1 */
    Cell prev[50][80];
    int force;
    char out[400000];
} Render;
void render_init(Render *r);
/* mode: 0 = ST3 hand map + CP437 + auto for block 1; 1 = automatic block elements everywhere */
void render_build_map(Render *r, const uint8_t *font, int charh, int mode);
int render_frame(Render *r, const uint8_t *vram, int rows, const uint8_t *dac, const uint8_t *attr, uint8_t seq3,
                 int term_rows, int term_cols, int y0, int x0);
#endif
