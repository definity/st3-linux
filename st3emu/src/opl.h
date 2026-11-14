/* opl.h — Yamaha YM3812 (OPL2) FM synthesizer emulation, as found on AdLib and Sound Blaster cards.
 * Runs at the chip's native rate (3.579545 MHz / 72 = 49716 Hz).  Original implementation written from the
 * documented behaviour of the chip (log-sine/exponent tables, envelope rate tables, LFOs, rhythm mode). */
#ifndef ST3_OPL_H
#define ST3_OPL_H
#include <stdint.h>

#define OPL_RATE (3579545.0 / 72.0)

typedef struct {
    /* registers */
    uint8_t am, vib, egt, ksr, mult;   /* 20h */
    uint8_t ksl, tl;                   /* 40h */
    uint8_t ar, dr, sl, rr;            /* 60h, 80h */
    uint8_t ws;                        /* E0h */
    /* state */
    uint32_t phase;                    /* 19.x fixed: top 10 bits of (phase >> 9) index the wave */
    int env;                           /* 0 (loud) .. 511 (silent) */
    int state;                         /* EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE */
    int key;                           /* bit0: channel key-on, bit1: rhythm key-on */
    int out, prev;                     /* last two outputs (feedback) */
} OplOp;

typedef struct {
    uint16_t fnum; uint8_t block, kon, fb, cnt;
} OplCh;

typedef struct {
    OplOp op[18];
    OplCh ch[9];
    uint8_t reg01, reg08, regbd;
    uint8_t addr;
    /* timers */
    uint8_t t1, t2, tctl, status;
    double t1_next, t2_next;
    /* global counters */
    uint32_t eg_cnt, sample;
    uint32_t noise;
    int tc_bits;                       /* top-cymbal phase bits for rhythm phase generation */
} Opl;

void opl_reset(Opl *o);
void opl_write_addr(Opl *o, uint8_t a);
void opl_write_data(Opl *o, uint8_t v, double now);
uint8_t opl_read_status(Opl *o, double now);
int opl_sample(Opl *o);   /* advance one native sample, return mono output (about +-4084 per voice, summed) */
int opl_active(const Opl *o);   /* any operator audible? */
#endif
