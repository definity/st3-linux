/* flicker.c — look for transient pictures: bring ST3 into a state, then press keys and render the exact VGA picture
 * every 1/280 s (4x the 70 Hz refresh).  Any frame that differs from both the picture before the key and the settled
 * picture after it is a transient; its differing pixel rows are reported (and saved as PPM with KP_PPM=prefix).
 * usage: flicker ROOT EXE "SETUP-KEYSCRIPT" "PROBE-KEYSCRIPT" */
#include "../src/machine.h"
#include "../src/keys.h"
#include "../src/vgafb.h"
#include <stdlib.h>
#include <string.h>

static uint8_t MEM[MEM_SIZE];
static Machine M;
static VgaFrame A, B, F[400];

static void run_for(double secs) { double e = machine_vtime(&M) + secs; while (machine_vtime(&M) < e) machine_run(&M, 2000); }
static void save(const char *pre, int k, VgaFrame *f) {
    char p[512]; snprintf(p, sizeof p, "%s%03d.ppm", pre, k); FILE *o = fopen(p, "wb"); if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", f->w, f->h); for (int i = 0; i < f->w * f->h; i++) fwrite(f->pal + f->pix[i] * 3, 1, 3, o); fclose(o);
}

int main(int argc, char **argv) {
    MachineConfig cfg = {0}; cfg.mips = 25.0; cfg.root = argv[1]; cfg.args = "";
    machine_init(&M, MEM, &cfg, NULL);
    if (machine_load_exe(&M, argv[2])) return 1;
    KeyScript ks; keyscript_parse(&ks, argv[3]);
    run_for(2.0);
    for (int i = 0; i < ks.n; i++) { if (ks.tok[i].wait_ms) run_for(ks.tok[i].wait_ms / 1000.0); else { machine_key(&M, ks.tok[i].key); run_for(0.3); } }
    run_for(1.0);
    keyscript_parse(&ks, argv[4]);
    int transients = 0, saved = 0;
    for (int i = 0; i < ks.n; i++) {
        if (ks.tok[i].wait_ms) { run_for(ks.tok[i].wait_ms / 1000.0); continue; }
        vgafb_render(&M, &A, 0);
        machine_key(&M, ks.tok[i].key);
        int nf = 0;
        for (; nf < 140; nf++) { run_for(1.0 / 280); vgafb_render(&M, &F[nf], 0); }   /* 0.5 s */
        B = F[nf - 1];
        for (int k = 0; k < nf; k++) {
            int y0 = -1, y1 = -1, blackrows = 0;
            if (!memcmp(F[k].pix, A.pix, A.w * A.h) || !memcmp(F[k].pix, B.pix, B.w * B.h)) continue;
            /* differs from before and after: is it just an intermediate step (rows equal to before or after)? */
            for (int y = 0; y < B.h; y++) {
                const uint8_t *r = F[k].pix + y * B.w;
                if (!memcmp(r, A.pix + y * B.w, B.w) || !memcmp(r, B.pix + y * B.w, B.w)) continue;
                if (y0 < 0) y0 = y; y1 = y;
                int blk = 1; for (int x = 0; x < B.w; x++) if (F[k].pal[r[x] * 3] | F[k].pal[r[x] * 3 + 1] | F[k].pal[r[x] * 3 + 2]) { blk = 0; break; }
                blackrows += blk;
            }
            if (y0 < 0) continue;   /* every row matches before or after: a partial (top/bottom) update, not garbage */
            transients++;
            printf("key %d frame %d (+%.1f ms): rows %d..%d differ from before AND after, %d fully black\n", i, k, (k + 1) * 1000.0 / 280, y0, y1, blackrows);
            if (getenv("KP_PPM") && saved < 20) save(getenv("KP_PPM"), saved++, &F[k]);
        }
    }
    printf("%d transient frames\n", transients);
    return 0;
}
