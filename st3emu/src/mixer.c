/* mixer.c — the sound card's analog output stage: SB Pro DAC + YM3812 FM, mixed into one 48 kHz s16 stereo stream.
 * Clocked by virtual time: mixer_update() emits frames up to machine_vtime()*48000.  The DAC bytes arrive from
 * sb_update() (resampled here, a stereo frame may be split across calls); FM is generated at the chip's native
 * 49716 Hz and linearly resampled.  FM is mono (ST3 writes 388h/389h, which on an SB Pro feeds both OPL2s). */
#include "machine.h"
#include <string.h>

#define DAC_GAIN 30000.0f   /* 8-bit full scale -> ~-0.8 dBFS */
#define FM_GAIN 1           /* chip sums 9 voices (+-4084 each) into 16 bits, fed straight to its DAC */
#define DACQ_FRAMES (int)(sizeof m->mix.dacq / sizeof m->mix.dacq[0] / 2)

void mixer_dac(Machine *m, const uint8_t *d, int n, double rate, int stereo) {
    int ch = stereo ? 2 : 1;
    double step = rate / MIX_RATE;
    if (stereo != m->mix.stereo_prev) { m->mix.part_n = 0; m->mix.stereo_prev = stereo; }
    for (int i = 0; i < n; i++) {
        m->mix.part[m->mix.part_n++] = d[i];
        if (m->mix.part_n < ch) continue;
        m->mix.part_n = 0;
        float l = (m->mix.part[0] - 128) / 128.0f, r = stereo ? (m->mix.part[1] - 128) / 128.0f : l;
        while (m->mix.phase <= 1.0) {
            float ol = m->mix.pl + (l - m->mix.pl) * (float)m->mix.phase, orr = m->mix.pr + (r - m->mix.pr) * (float)m->mix.phase;
            if (m->mix.dq_n < DACQ_FRAMES) {
                m->mix.dacq[m->mix.dq_n * 2] = (int16_t)(ol * DAC_GAIN);
                m->mix.dacq[m->mix.dq_n * 2 + 1] = (int16_t)(orr * DAC_GAIN);
                m->mix.dq_n++;
            }
            m->mix.phase += step;
        }
        m->mix.phase -= 1.0;
        m->mix.pl = l; m->mix.pr = r;
    }
}

static void flush(Machine *m) {
    if (m->mix.on && m->pcm_sink) m->pcm_sink(m->pcm_ud, m->mix.out, m->mix.on / 2);
    m->mix.on = 0;
}

void mixer_update(Machine *m) {
    sb_update(m);   /* bring the DAC up to now first */
    if (!m->pcm_sink) { m->mix.dq_n = 0; return; }
    uint64_t target = (uint64_t)(machine_vtime(m) * MIX_RATE);
    if (target <= m->mix.frames) return;
    int n = (int)(target - m->mix.frames), avail = m->mix.dq_n;
    /* while the DAC runs, its frames trail virtual time by a frame or two (byte granularity): wait for them,
     * unless it has fallen far behind (transfer restarted / stopped), then pad with silence */
    if (m->sb.running && avail < n && n - avail < 512) n = avail;
    if (n <= 0) return;
    int fm_on = opl_active(&m->opl);
    double fstep = OPL_RATE / MIX_RATE;
    for (int i = 0; i < n; i++) {
        int l = 0, r = 0;
        if (i < avail) { l = m->mix.dacq[i * 2]; r = m->mix.dacq[i * 2 + 1]; }
        if (fm_on) {
            m->mix.opl_pos += fstep;
            while (m->mix.opl_pos >= 1.0) { m->mix.opl_prev = m->mix.opl_cur; m->mix.opl_cur = opl_sample(&m->opl); m->mix.opl_pos -= 1.0; }
            int fm = (int)(m->mix.opl_prev + (m->mix.opl_cur - m->mix.opl_prev) * m->mix.opl_pos) * FM_GAIN;
            l += fm; r += fm;
        }
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        m->mix.out[m->mix.on++] = (int16_t)l; m->mix.out[m->mix.on++] = (int16_t)r;
        if (m->mix.on >= (int)(sizeof m->mix.out / sizeof m->mix.out[0])) flush(m);
    }
    flush(m);
    int used = n < avail ? n : avail;
    m->mix.dq_n -= used;
    if (m->mix.dq_n > 1024) m->mix.dq_n = 0;   /* DAC ran ahead (should not happen): resync */
    if (m->mix.dq_n) memmove(m->mix.dacq, m->mix.dacq + used * 2, m->mix.dq_n * 2 * sizeof m->mix.dacq[0]);
    m->mix.frames += n;
}
