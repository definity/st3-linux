/* opl.c — YM3812 (OPL2) emulation.  See opl.h.
 *
 * Signal path per operator (as in the chip):
 *   phase generator: phase += ((fnum << block) >> 1) * MULT / 2 per sample (with vibrato applied to fnum);
 *                    10-bit wave index = phase >> 9 (+ modulation input).
 *   waveform:        quarter-wave log-sine ROM -> attenuation (log2 domain, 1/256 octave units)
 *   envelope:        9-bit attenuation, 0.1875 dB/step; ADSR with the chip's rate tables (4 sub-steps per rate)
 *   level:           env + TL*4 + KSL + tremolo  (all in 0.1875 dB units), added to the log-sine value (x8)
 *   output:          exponent ROM -> linear 12-bit magnitude + sign
 * Channels: 2-op FM (mod->car) or additive; feedback on the modulator; rhythm mode on channels 6-8. */
#include "opl.h"
#include <math.h>
#include <string.h>

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

static uint16_t logsin_tab[256], exp_tab[256];
static int tables_ready;

static const uint8_t mult2[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};   /* MULT x2 */
static const uint8_t ksl_rom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const uint8_t ksl_shift[4] = {8, 1, 2, 0};   /* KSL 0: off, 1: 3 dB/oct, 2: 1.5 dB/oct, 3: 6 dB/oct */
/* envelope increment patterns, 8 steps each (selected by rate) */
static const uint8_t eg_inc[15][8] = {
    {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2},
    {2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 4, 2, 2, 2, 4}, {2, 4, 2, 4, 2, 4, 2, 4}, {2, 4, 4, 4, 2, 4, 4, 4},
    {4, 4, 4, 4, 4, 4, 4, 4}, {8, 8, 8, 8, 8, 8, 8, 8}, {0, 0, 0, 0, 0, 0, 0, 0},
};
/* register offset (low 5 bits) -> operator slot, -1 = none */
static const int8_t slot_of[32] = {0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
                                   12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static const uint8_t ch_mod[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};   /* carrier = modulator + 3 */
static int slot_ch(int s) { return (s / 6) * 3 + (s % 6) % 3; }

static void init_tables(void) {
    for (int i = 0; i < 256; i++) {
        logsin_tab[i] = (uint16_t)lround(-log2(sin((i + 0.5) * M_PI / 512.0)) * 256.0);
        exp_tab[i] = (uint16_t)(lround(pow(2.0, (255 - i) / 256.0) * 1024.0) - 1024);
    }
    tables_ready = 1;
}

void opl_reset(Opl *o) {
    if (!tables_ready) init_tables();
    memset(o, 0, sizeof *o);
    for (int i = 0; i < 18; i++) { o->op[i].env = 511; o->op[i].state = EG_RELEASE; }
    o->noise = 1;
}

/* wave output for a 10-bit phase and 9-bit envelope attenuation */
static int wave(int phase, int env, int ws) {
    int p = phase & 1023, idx, neg = 0;
    switch (ws) {
    default:
    case 0: idx = (p & 0x100) ? (~p & 0xFF) : (p & 0xFF); neg = p & 0x200; break;    /* sine */
    case 1: if (p & 0x200) return 0; idx = (p & 0x100) ? (~p & 0xFF) : (p & 0xFF); break;   /* half sine */
    case 2: idx = (p & 0x100) ? (~p & 0xFF) : (p & 0xFF); break;                     /* abs sine */
    case 3: if (p & 0x100) return 0; idx = p & 0xFF; break;                          /* quarter (pulse) sine */
    }
    int x = logsin_tab[idx] + (env << 3);
    if (x > 0x1FFF) x = 0x1FFF;
    int v = ((exp_tab[x & 0xFF] | 0x400) << 1) >> (x >> 8);
    return neg ? ~v : v;
}

static int eff_rate(const Opl *o, const OplOp *op, int ch, int r) {
    if (!r) return 0;
    const OplCh *c = &o->ch[ch];
    int ksv = (c->block << 1) | ((c->fnum >> ((o->reg08 & 0x40) ? 8 : 9)) & 1);
    int rr = r * 4 + (op->ksr ? ksv : ksv >> 2);
    return rr > 63 ? 63 : rr;
}

static int eg_step(const Opl *o, int rate) {   /* increment for this sample at this rate (0 if not an update tick) */
    if (rate < 4) return 0;
    int g = rate >> 2, sel, shift = g < 13 ? 12 - g : 0;
    if (g <= 12) sel = rate & 3; else if (g == 13) sel = 4 + (rate & 3); else if (g == 14) sel = 8 + (rate & 3); else sel = 12;
    if (o->eg_cnt & ((1u << shift) - 1)) return 0;
    return eg_inc[sel][(o->eg_cnt >> shift) & 7];
}

static void key_change(Opl *o, int s, int bit, int on) {
    OplOp *op = &o->op[s];
    int was = op->key;
    if (on) op->key |= bit; else op->key &= ~bit;
    if (!was && op->key) {   /* key on: restart envelope and phase */
        op->state = EG_ATTACK; op->phase = 0;
        if (eff_rate(o, op, slot_ch(s), op->ar) >= 60) { op->env = 0; op->state = EG_DECAY; }
    } else if (was && !op->key) op->state = EG_RELEASE;
}

static void envelope(Opl *o, int s) {
    OplOp *op = &o->op[s];
    int ch = slot_ch(s), inc;
    switch (op->state) {
    case EG_ATTACK:
        inc = eg_step(o, eff_rate(o, op, ch, op->ar));
        if (inc) { op->env += (~op->env * inc) >> 3; if (op->env <= 0) { op->env = 0; op->state = EG_DECAY; } }
        break;
    case EG_DECAY: {
        int sl = op->sl == 15 ? 496 : op->sl * 16;
        op->env += eg_step(o, eff_rate(o, op, ch, op->dr));
        if (op->env >= sl) op->state = EG_SUSTAIN;
        break; }
    case EG_SUSTAIN:
        if (!op->egt) op->env += eg_step(o, eff_rate(o, op, ch, op->rr));   /* percussive sound: keep decaying */
        break;
    case EG_RELEASE:
        op->env += eg_step(o, eff_rate(o, op, ch, op->rr));
        break;
    }
    if (op->env > 511) op->env = 511;
}

static int level(const Opl *o, const OplOp *op, int ch) {   /* total attenuation, 0..511 */
    const OplCh *c = &o->ch[ch];
    int ksl = (ksl_rom[c->fnum >> 6] << 2) - ((8 - c->block) << 5);
    if (ksl < 0) ksl = 0;
    ksl >>= ksl_shift[op->ksl];
    int e = op->env + (op->tl << 2) + ksl;
    if (op->am) {
        int tp = (o->sample >> 6) % 210, t = tp < 105 ? tp : 210 - tp;
        e += t >> ((o->regbd & 0x80) ? 2 : 4);
    }
    return e > 511 ? 511 : e;
}

static uint32_t phase_inc(const Opl *o, const OplOp *op, int ch) {
    const OplCh *c = &o->ch[ch];
    int f = c->fnum;
    if (op->vib) {
        int vp = (o->sample >> 10) & 7, r = (f >> 7) & 7;
        if (!(vp & 3)) r = 0; else if (vp & 1) r >>= 1;
        if (!(o->regbd & 0x40)) r >>= 1;
        f += (vp & 4) ? -r : r;
    }
    return ((((uint32_t)f << c->block) >> 1) * mult2[op->mult]) >> 1;
}

static int ws_of(const Opl *o, const OplOp *op) { return (o->reg01 & 0x20) ? op->ws & 3 : 0; }

static int op_calc(Opl *o, int s, int mod) {   /* compute operator output with phase-modulation input `mod` */
    OplOp *op = &o->op[s];
    int ch = slot_ch(s);
    int v = wave((int)(op->phase >> 9) + mod, level(o, op, ch), ws_of(o, op));
    op->prev = op->out; op->out = v;
    return v;
}

int opl_sample(Opl *o) {
    int out = 0, rhythm = o->regbd & 0x20;
    int nch = rhythm ? 6 : 9;
    for (int ch = 0; ch < nch; ch++) {
        OplCh *c = &o->ch[ch];
        int m = ch_mod[ch], k = m + 3;
        OplOp *mo = &o->op[m];
        int fb = c->fb ? (mo->out + mo->prev) >> (9 - c->fb) : 0;
        int mv = op_calc(o, m, fb);
        int cv = op_calc(o, k, c->cnt ? 0 : mv);
        out += c->cnt ? mv + cv : cv;
    }
    if (rhythm) {
        /* bass drum: channel 6 as a normal 2-op voice (carrier only in additive mode), double level */
        OplCh *c6 = &o->ch[6];
        OplOp *bm = &o->op[12];
        int fb = c6->fb ? (bm->out + bm->prev) >> (9 - c6->fb) : 0;
        int mv = op_calc(o, 12, fb);
        out += 2 * op_calc(o, 15, c6->cnt ? 0 : mv);
        /* hi-hat, snare, top cymbal: phases derived from slot 13 (HH) and 17 (TC) phase bits plus noise */
        int hh = (int)(o->op[13].phase >> 9), tc = (int)(o->op[17].phase >> 9), nz = o->noise & 1;
        int hb2 = (hh >> 2) & 1, hb3 = (hh >> 3) & 1, hb7 = (hh >> 7) & 1, hb8 = (hh >> 8) & 1;
        int tb3 = (tc >> 3) & 1, tb5 = (tc >> 5) & 1;
        int x = (hb2 ^ hb7) | (hb3 ^ tb5) | (tb3 ^ tb5);
        int ph_hh = (x << 9) | ((x ^ nz) ? 0xD0 : 0x34);
        int ph_sd = (hb8 << 9) | ((hb8 ^ nz) << 8);
        int ph_tc = (x << 9) | 0x80;
        OplOp *h = &o->op[13], *sd = &o->op[16], *tom = &o->op[14], *cy = &o->op[17];
        out += 2 * wave(ph_hh, level(o, h, 7), ws_of(o, h));
        out += 2 * wave(ph_sd, level(o, sd, 7), ws_of(o, sd));
        out += 2 * wave((int)(tom->phase >> 9), level(o, tom, 8), ws_of(o, tom));
        out += 2 * wave(ph_tc, level(o, cy, 8), ws_of(o, cy));
    }
    /* advance phase and envelope generators, LFO and noise */
    for (int s = 0; s < 18; s++) {
        OplOp *op = &o->op[s];
        op->phase = (op->phase + phase_inc(o, op, slot_ch(s))) & 0x7FFFF;
        envelope(o, s);
    }
    uint32_t n = o->noise;
    o->noise = (n >> 1) | ((((n) ^ (n >> 14)) & 1) << 22);
    o->eg_cnt++; o->sample++;
    return out;
}

int opl_active(const Opl *o) {
    for (int s = 0; s < 18; s++) if (o->op[s].env < 500 || o->op[s].state != EG_RELEASE) return 1;
    return 0;
}

void opl_write_addr(Opl *o, uint8_t a) { o->addr = a; }

static void timer_catchup(Opl *o, double now) {
    if (o->tctl & 1) {
        double per = (256 - o->t1) * 80e-6;
        while (now >= o->t1_next) { if (!(o->tctl & 0x40)) o->status |= 0xC0; o->t1_next += per; }
    }
    if (o->tctl & 2) {
        double per = (256 - o->t2) * 320e-6;
        while (now >= o->t2_next) { if (!(o->tctl & 0x20)) o->status |= 0xA0; o->t2_next += per; }
    }
}

uint8_t opl_read_status(Opl *o, double now) { timer_catchup(o, now); return (o->status & 0xE0) | 0x06; }

void opl_write_data(Opl *o, uint8_t v, double now) {
    uint8_t r = o->addr;
    int s = slot_of[r & 0x1F];
    switch (r & 0xE0) {
    case 0x20: if (s >= 0 && r < 0x36) { OplOp *op = &o->op[s]; op->am = v >> 7; op->vib = (v >> 6) & 1; op->egt = (v >> 5) & 1; op->ksr = (v >> 4) & 1; op->mult = v & 15; } return;
    case 0x40: if (s >= 0 && r < 0x56) { o->op[s].ksl = v >> 6; o->op[s].tl = v & 63; } return;
    case 0x60: if (s >= 0 && r < 0x76) { o->op[s].ar = v >> 4; o->op[s].dr = v & 15; } return;
    case 0x80: if (s >= 0 && r < 0x96) { o->op[s].sl = v >> 4; o->op[s].rr = v & 15; } return;
    case 0xE0: if (s >= 0 && r < 0xF6) o->op[s].ws = v & 3; return;
    }
    if (r >= 0xA0 && r <= 0xA8) { o->ch[r - 0xA0].fnum = (o->ch[r - 0xA0].fnum & 0x300) | v; return; }
    if (r >= 0xB0 && r <= 0xB8) {
        int ch = r - 0xB0; OplCh *c = &o->ch[ch];
        c->fnum = (c->fnum & 0xFF) | ((v & 3) << 8); c->block = (v >> 2) & 7; c->kon = (v >> 5) & 1;
        key_change(o, ch_mod[ch], 1, c->kon); key_change(o, ch_mod[ch] + 3, 1, c->kon);
        return;
    }
    if (r >= 0xC0 && r <= 0xC8) { o->ch[r - 0xC0].fb = (v >> 1) & 7; o->ch[r - 0xC0].cnt = v & 1; return; }
    switch (r) {
    case 0x01: o->reg01 = v; return;
    case 0x02: o->t1 = v; return;
    case 0x03: o->t2 = v; return;
    case 0x04:
        timer_catchup(o, now);
        if (v & 0x80) { o->status = 0; return; }
        if ((v & 1) && !(o->tctl & 1)) o->t1_next = now + (256 - o->t1) * 80e-6;
        if ((v & 2) && !(o->tctl & 2)) o->t2_next = now + (256 - o->t2) * 320e-6;
        o->tctl = v;
        return;
    case 0x08: o->reg08 = v; return;
    case 0xBD: {
        o->regbd = v;
        int rh = v & 0x20;
        key_change(o, 12, 2, rh && (v & 0x10)); key_change(o, 15, 2, rh && (v & 0x10));   /* BD */
        key_change(o, 16, 2, rh && (v & 0x08));                                           /* SD */
        key_change(o, 14, 2, rh && (v & 0x04));                                           /* TOM */
        key_change(o, 17, 2, rh && (v & 0x02));                                           /* CY */
        key_change(o, 13, 2, rh && (v & 0x01));                                           /* HH */
        return; }
    }
}
