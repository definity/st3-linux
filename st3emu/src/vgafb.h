#ifndef ST3_VGAFB_H
#define ST3_VGAFB_H
#include "machine.h"
#define VGAFB_MAXW 720
#define VGAFB_MAXH 480
typedef struct { int w, h; uint8_t pix[VGAFB_MAXW * VGAFB_MAXH]; uint8_t pal[768]; } VgaFrame;
void vgafb_geometry(const Machine *m, int *w, int *h);
/* render the current text screen; `frame` = vertical retrace count (drives blink: cursor /16, characters /32) */
void vgafb_render(const Machine *m, VgaFrame *f, unsigned frame);
#endif
