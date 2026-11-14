/* st3web.c — browser front end for the ST3 emulator (WebAssembly).  The page (index.html + st3.js) drives it:
 *   st3_init()                        set up the machine (C:\ST3 = /st3 in the browser filesystem) and load ST3
 *   st3_frame(now_s, queued_s)        advance the emulator; paced by the audio queue (queued_s >= 0) or the wall clock
 *   st3_render()                      draw the exact VGA picture into an RGBA buffer, returns 1 if it changed
 *   st3_key(biosKey) / st3_shift(f)   keyboard: BIOS key codes and the live Shift/Ctrl/Alt state from key up/down
 *   st3_audio_take()                  mixed audio produced since the last call (48 kHz stereo float, interleaved)
 * The emulator core (st3emu/src) is shared unchanged with the Linux terminal version. */
#include "../st3emu/src/machine.h"
#include "../st3emu/src/vgafb.h"
#include <emscripten.h>
#include <string.h>

extern const unsigned char st3_exe[]; extern const unsigned int st3_exe_len;

static uint8_t MEM[MEM_SIZE];
static Machine M;
static VgaFrame VF;
static uint32_t rgba[VGAFB_MAXW * VGAFB_MAXH];
static uint32_t lastsum;
static double t0 = -1;
static int started;

#define AQ_FRAMES 48000                    /* 1 s of audio between frames is plenty */
static float aq[AQ_FRAMES * 2]; static int aq_n;

static void sink(void *ud, const int16_t *f, int n) {
    for (int i = 0; i < n && aq_n < AQ_FRAMES; i++, aq_n++) { aq[aq_n * 2] = f[i * 2] / 32768.0f; aq[aq_n * 2 + 1] = f[i * 2 + 1] / 32768.0f; }
}

EMSCRIPTEN_KEEPALIVE int st3_init(void) {
    static MachineConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.mips = 25.0; cfg.root = "/st3"; cfg.args = "";
    machine_init(&M, MEM, &cfg, NULL);
    if (machine_load_exe_mem(&M, st3_exe, (long)st3_exe_len, "ST3.EXE")) return -1;
    M.pcm_sink = sink; M.pcm_ud = NULL;
    started = 1;
    return 0;
}

#define AUDIO_TARGET 0.10                  /* seconds of audio kept queued ahead of the device */
/* queued: seconds of audio waiting in the page's audio player, or < 0 when there is no sound (yet) */
EMSCRIPTEN_KEEPALIVE int st3_frame(double now, double queued) {
    if (!started || M.exited) return M.exited ? 2 : 0;
    if (t0 < 0) t0 = now;
    double wall = now - t0, vt = machine_vtime(&M), target = wall;
    if (queued >= 0) {                     /* the audio device is the clock (same scheme as the terminal version) */
        target = vt + (AUDIO_TARGET - queued);
        if (target > wall + 0.15) target = wall + 0.15;
        if (target < wall - 0.2) target = wall - 0.2;
    }
    if (target > vt + 0.1) target = vt + 0.1;   /* at most 0.1 s of emulation per call */
    if (vt < target && machine_run(&M, (uint64_t)((target - vt) * 25.0e6) + 1) < 0) return -1;
    vt = machine_vtime(&M);
    if (wall - vt > 0.25) t0 += (wall - vt) - 0.05;   /* tab was in the background / host too slow: skip ahead */
    return M.exited ? 2 : 1;
}

EMSCRIPTEN_KEEPALIVE int st3_audio_take(void) { int n = aq_n; aq_n = 0; return n; }
EMSCRIPTEN_KEEPALIVE float *st3_audio_buf(void) { return aq; }

EMSCRIPTEN_KEEPALIVE int st3_render(void) {
    if (!started || M.fontmode || M.font_uploads == 0) return 0;   /* ST3 shows its own fonts within ~1 s of start */
    vgafb_render(&M, &VF, (unsigned)(machine_vtime(&M) * 70.086));
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u | VF.pal[i * 3 + 2] << 16 | VF.pal[i * 3 + 1] << 8 | VF.pal[i * 3];
    uint32_t sum = 2166136261u ^ (uint32_t)(VF.w * 7 + VF.h);
    for (int i = 0; i < VF.w * VF.h; i++) { uint32_t c = pal[VF.pix[i]]; rgba[i] = c; sum = (sum ^ c) * 16777619u; }
    if (sum == lastsum) return 0;
    lastsum = sum; return 1;
}
EMSCRIPTEN_KEEPALIVE uint32_t *st3_pixels(void) { return rgba; }
EMSCRIPTEN_KEEPALIVE int st3_width(void) { return VF.w; }
EMSCRIPTEN_KEEPALIVE int st3_height(void) { return VF.h; }

EMSCRIPTEN_KEEPALIVE void st3_key(int k) { if (k && machine_keyq_free(&M) > 0) machine_key(&M, (uint16_t)k); }
EMSCRIPTEN_KEEPALIVE void st3_shift(int f) { M.shift_flags = (uint16_t)f; }
EMSCRIPTEN_KEEPALIVE int st3_exited(void) { return M.exited; }
