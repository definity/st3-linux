/* opl_test.c — sanity checks of the OPL2 emulation against YM3812 datasheet figures.
 * build: cc -O2 -o opl_test tests/opl_test.c src/opl.c -lm ; run: ./opl_test [wavfile] */
#include "../src/opl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void w(Opl *o, int r, int v) { opl_write_addr(o, r); opl_write_data(o, v, 0); }
static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

/* one carrier-only voice on channel 0 (modulator silent), additive */
static void voice(Opl *o, int ar, int dr, int sl, int rr, int egt) {
    opl_reset(o);
    w(o, 0x01, 0x20);
    w(o, 0x20, 0x01); w(o, 0x40, 0x3F);                    /* modulator: silent */
    w(o, 0x23, 0x01 | (egt ? 0x20 : 0)); w(o, 0x43, 0x00); /* carrier: MULT 1, TL 0 */
    w(o, 0x63, (ar << 4) | dr); w(o, 0x83, (sl << 4) | rr);
    w(o, 0xC0, 0x01);
}

int main(int argc, char **argv) {
    Opl o;
    /* 1. pitch: A4 = 440 Hz -> fnum = 440 * 2^20 / (49716 * 2^4) = 580 at block 4 */
    voice(&o, 15, 0, 0, 0, 1);
    w(&o, 0xA0, 580 & 0xFF); w(&o, 0xB0, 0x20 | (4 << 2) | (580 >> 8));
    int prev = 0, cross = 0, first = -1, last = -1, peak = 0, N = (int)OPL_RATE;
    for (int i = 0; i < N; i++) {
        int v = opl_sample(&o);
        if (abs(v) > peak) peak = abs(v);
        if (prev < 0 && v >= 0) { if (first < 0) first = i; last = i; cross++; }
        prev = v;
    }
    double f = (cross - 1) * OPL_RATE / (last - first);
    CHECK(fabs(f - 440.0) < 0.5, "pitch fnum=580 block=4: %.2f Hz (expect 440.0)", f);
    CHECK(peak >= 4000 && peak <= 4096, "peak amplitude TL=0: %d (expect ~4084)", peak);

    /* 2. decay times 0 -> -96 dB (env 0 -> 511) vs datasheet: 39280 ms at rate 1, halving per step */
    for (int dr = 1; dr <= 12; dr += 1) {
        voice(&o, 15, dr, 15, 0, 1);
        w(&o, 0xA0, 0x00); w(&o, 0xB0, 0x20);   /* block 0, fnum 0 => key scale offset 0 */
        long n = 0; int lim = (int)(OPL_RATE * 60);
        while (n < lim) { opl_sample(&o); n++; if (o.op[3].env >= 496) break; }
        double ms = n * 1000.0 / OPL_RATE * 512.0 / 496.0, ref = 39280.0 / pow(2, dr - 1);
        CHECK(fabs(ms / ref - 1) < 0.12, "decay rate %2d: %9.1f ms (datasheet %9.1f)", dr, ms, ref);
    }
    /* 3. attack times 100%: datasheet 2826 ms at rate 1 halving per step; rate 15 = 0 */
    for (int ar = 1; ar <= 15; ar += 2) {
        voice(&o, ar, 0, 0, 0, 1);
        w(&o, 0xA0, 0x00); w(&o, 0xB0, 0x20);
        long n = 0; int lim = (int)(OPL_RATE * 10);
        while (n < lim && o.op[3].env > 0) { opl_sample(&o); n++; }
        double ms = n * 1000.0 / OPL_RATE, ref = ar == 15 ? 0 : 2826.0 / pow(2, ar - 1);
        CHECK(ar == 15 ? ms == 0 : fabs(ms / ref - 1) < 0.25, "attack rate %2d: %8.2f ms (datasheet %8.2f)", ar, ms, ref);
    }
    /* 4. FM voice + rhythm smoke test, optional WAV */
    opl_reset(&o);
    w(&o, 0x01, 0x20);
    w(&o, 0x20, 0x01); w(&o, 0x40, 0x10); w(&o, 0x60, 0xF2); w(&o, 0x80, 0x77);
    w(&o, 0x23, 0x01); w(&o, 0x43, 0x00); w(&o, 0x63, 0xF2); w(&o, 0x83, 0x77);
    w(&o, 0xC0, 0x0C);
    w(&o, 0xA0, 0x44); w(&o, 0xB0, 0x32);
    int n2 = (int)OPL_RATE * 2; short *buf = malloc(n2 * 2); long sum = 0;
    for (int i = 0; i < n2; i++) {
        if (i == n2 / 2) { w(&o, 0xB0, 0x12); w(&o, 0xA6, 0x57); w(&o, 0xB6, 0x09); w(&o, 0xBD, 0x3F); }
        int v = opl_sample(&o); buf[i] = (short)(v * 4); sum += abs(v);
    }
    CHECK(sum > 0, "FM + rhythm produce output (mean |x| %.0f)", (double)sum / n2);
    if (argc > 1) {
        FILE *f2 = fopen(argv[1], "wb"); unsigned rate = 49716, by = n2 * 2, v;
        fwrite("RIFF", 1, 4, f2); v = 36 + by; fwrite(&v, 4, 1, f2); fwrite("WAVEfmt ", 1, 8, f2);
        v = 16; fwrite(&v, 4, 1, f2); short s = 1; fwrite(&s, 2, 1, f2); fwrite(&s, 2, 1, f2);
        fwrite(&rate, 4, 1, f2); v = rate * 2; fwrite(&v, 4, 1, f2); s = 2; fwrite(&s, 2, 1, f2); s = 16; fwrite(&s, 2, 1, f2);
        fwrite("data", 1, 4, f2); fwrite(&by, 4, 1, f2); fwrite(buf, 2, n2, f2); fclose(f2);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails != 0;
}
