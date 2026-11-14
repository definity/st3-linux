/* headless.c — run ST3 in the C emulator without a terminal: scripted keys, WAV capture, VGA dump.
 * usage: st3head ROOT EXE SECONDS "KEYSCRIPT" OUTPREFIX [args...]
 *   KEYSCRIPT tokens: plain chars, {ESC} {ENTER} {TAB} {BS} {UP} {DOWN} {LEFT} {RIGHT} {PGUP} {PGDN} {HOME} {END}
 *                     {INS} {DEL} {F1}..{F12} {ALT-x} {CTRL-x} {Wn}=wait n ms (virtual; default 300)
 * outputs: OUTPREFIX.wav, OUTPREFIX.json + OUTPREFIX.font (for tools/render.py), OUTPREFIX.log */
#include "../src/machine.h"
#include "../src/keys.h"
#include "../src/vgafb.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t MEM[MEM_SIZE];
static Machine M;

typedef struct { uint8_t *buf; size_t n, cap; double rate; int stereo; } Wav;
static void sink(void *ud, const int16_t *f, int frames) {   /* mixer output: 48 kHz s16 stereo */
    Wav *w = ud; size_t n = (size_t)frames * 4;
    if (w->n + n > w->cap) { w->cap = (w->n + n) * 2 + 65536; w->buf = realloc(w->buf, w->cap); }
    memcpy(w->buf + w->n, f, n); w->n += n; w->rate = MIX_RATE; w->stereo = 1;
}
static void wav_write(const char *path, Wav *w) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    int ch = w->stereo ? 2 : 1; uint32_t rate = (uint32_t)(w->rate + 0.5), n = (uint32_t)w->n;
    uint8_t h[44]; memcpy(h, "RIFF", 4); uint32_t v = 36 + n; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); uint16_t s = 1; memcpy(h + 20, &s, 2); s = ch; memcpy(h + 22, &s, 2);
    memcpy(h + 24, &rate, 4); v = rate * ch * 2; memcpy(h + 28, &v, 4); s = ch * 2; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &n, 4);
    fwrite(h, 1, 44, f); fwrite(w->buf, 1, n, f); fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s ROOT EXE SECONDS KEYSCRIPT OUTPREFIX [args...]\n", argv[0]); return 1; }
    char path[1024];
    MachineConfig cfg = {0};
    cfg.mips = getenv("MIPS") ? atof(getenv("MIPS")) : 25.0;
    cfg.root = argv[1];
    cfg.no_ems = getenv("NOEMS") != NULL;
    static char args[256] = "";
    for (int i = 6; i < argc; i++) { strcat(args, i > 6 ? " " : ""); strncat(args, argv[i], 200); }
    cfg.args = args;
    snprintf(path, sizeof path, "%s.log", argv[5]);
    FILE *log = fopen(path, "w");
    machine_init(&M, MEM, &cfg, log);
    Wav wav = {0};
    M.pcm_sink = sink; M.pcm_ud = &wav;
    if (machine_load_exe(&M, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    double secs = atof(argv[3]);
    KeyScript ks; keyscript_parse(&ks, argv[4]);
    clock_t t0 = clock();
    double wait_until = 0;
    while (machine_vtime(&M) < secs && !M.exited) {
        if (machine_run(&M, 20000) < 0) { fprintf(stderr, "CPU error, see log\n"); break; }
        double vt = machine_vtime(&M);
        while (ks.pos < ks.n && vt >= wait_until && M.kq_head == M.kq_tail) {
            KeyTok *t = &ks.tok[ks.pos++];
            if (t->wait_ms) { wait_until = vt + t->wait_ms / 1000.0; break; }
            machine_key(&M, t->key);
        }
    }
    double wall = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "vtime %.2fs  insns %llu  wall %.2fs  (%.1f MIPS host)  exited=%d  font_uploads=%d  keys_left=%d\n",
            machine_vtime(&M), (unsigned long long)M.cpu.icount, wall, M.cpu.icount / wall / 1e6, M.exited, M.font_uploads, ks.n - ks.pos);
    snprintf(path, sizeof path, "%s.wav", argv[5]); if (wav.n) wav_write(path, &wav);
    char p2[1024];
    snprintf(path, sizeof path, "%s.json", argv[5]); snprintf(p2, sizeof p2, "%s.font", argv[5]);
    machine_dump_vga_state(&M, path, p2);
    if (getenv("DUMPMEM")) { FILE *mf = fopen(getenv("DUMPMEM"), "wb"); if (mf) { fwrite(MEM, 1, MEM_SIZE, mf); fclose(mf); } }
    {   /* exact VGA raster as a PPM */
        static VgaFrame F; vgafb_render(&M, &F, 0);
        snprintf(path, sizeof path, "%s.ppm", argv[5]);
        FILE *pf = fopen(path, "wb");
        if (pf) { fprintf(pf, "P6\n%d %d\n255\n", F.w, F.h);
                  for (int i = 0; i < F.w * F.h; i++) fwrite(F.pal + F.pix[i] * 3, 1, 3, pf); fclose(pf); }
    }
    return 0;
}
