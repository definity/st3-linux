/* machine.h — the emulated PC around the CPU: PIC, PIT, VGA text, SB Pro + DMA, BIOS/DOS HLE. */
#ifndef ST3_MACHINE_H
#define ST3_MACHINE_H
#include "cpu.h"
#include "opl.h"
#include <stdio.h>

#define STUBSEG 0xF000          /* IVT entries point at F000:nn; executing there traps to HLE */
#define PSP_SEG 0x0800
#define ENV_SEG 0x0700
#define MEM_TOP 0xA000
#define MAX_HANDLES 64
#define KEYQ 64

typedef struct {
    /* user-facing config */
    double mips;                 /* virtual instructions per second (millions) */
    const char *root;            /* host dir mapped to C:\ST3 */
    const char *args;            /* DOS command tail */
    char env[512]; int envlen;   /* extra env strings, NUL separated */
    int verbose;
} MachineConfig;

typedef struct Machine {
    CPU cpu;
    uint8_t *mem;
    MachineConfig cfg;
    FILE *log;

    /* time */
    uint64_t icount_base;

    /* PIC (master only; ST3 uses IRQ0 and IRQ7) */
    uint8_t pic_mask, irr, isr;
    /* PIT ch0 */
    uint32_t pit_div; int pit_lohi; uint8_t pit_lo; double pit_next; double pit_t0; uint8_t pit_latch_lo, pit_latch_hi; int pit_latched, pit_read_hi;
    uint8_t port61;

    /* VGA */
    uint8_t dac[768]; int dac_w, dac_r;
    uint8_t attr[32]; int attr_idx, attr_flip;
    uint8_t seq[8]; int seq_idx;
    uint8_t gc[16]; int gc_idx;
    uint8_t crtc[32]; int crtc_idx;
    uint8_t misc_out;
    uint8_t font[0x8000];        /* plane-2 window as seen at B800 (block0 @0, block1 @0x4000) */
    uint8_t saved_text[0x8000];
    int fontmode; uint32_t font_base;
    int font_uploads;
    int video_dirty;

    /* 8237 DMA (ch0-3) */
    struct { uint16_t addr, cnt; uint8_t page, mode, mask; uint32_t cur; } dma[4];   /* cur: bytes into the block (kept across DSP restarts) */
    int dma_ff;

    /* Sound Blaster Pro @ 0x220, IRQ7, DMA1 */
    struct {
        int rst;
        uint8_t outq[64]; int outn;
        int cmd, need, nargs; uint8_t args[4];
        uint8_t tc; uint16_t blk; int spk;
        int running, autoinit, highspeed;
        double t0; double bps; uint32_t len;   /* current DSP transfer */
        uint64_t consumed;                     /* bytes consumed in current transfer */
        uint8_t mix[256]; int mixidx;
        int irq_pending_f2; double f2_at;
    } sb;
    /* FM synthesizer (YM3812) at 388h/389h, also 220h-223h and 228h/229h */
    Opl opl;
    /* output mixer: SB DAC + FM -> 48 kHz s16 stereo, clocked by virtual time (mixer.c) */
    struct {
        double phase; float pl, pr; uint8_t part[2]; int part_n, stereo_prev;   /* DAC resampler state */
        int16_t dacq[8192 * 2]; int dq_n;                                         /* resampled DAC frames */
        double opl_pos; int opl_prev, opl_cur;                                    /* FM resampler state */
        float hold_l, hold_r;                                                     /* DAC output hold */
        uint64_t frames;                                                          /* output frames so far */
        int16_t out[1024 * 2]; int on;
    } mix;
    /* PCM sink: receives the mixed output, 48 kHz signed 16-bit stereo frames */
    void (*pcm_sink)(void *ud, const int16_t *frames, int nframes);
    void *pcm_ud;

    /* keyboard: BIOS-level queue of (scan<<8|ascii) */
    uint16_t keyq[KEYQ]; int kq_head, kq_tail;
    uint16_t shift_flags;

    /* DOS */
    FILE *handles[MAX_HANDLES]; int hkind[MAX_HANDLES];  /* 0 free, 1 file, 2 con */
    char cwd[256];               /* relative to root, DOS style, no leading backslash */
    uint16_t dta_seg, dta_off;
    struct { int n, pos; char (*names)[300]; } finds[8]; int find_slot_next;
    int exit_code, exited;

    /* stats */
    uint64_t ints_seen[256];
    int unhandled_reported;
} Machine;

void machine_init(Machine *m, uint8_t *mem, const MachineConfig *cfg, FILE *log);
int machine_load_exe(Machine *m, const char *path);
int machine_load_exe_mem(Machine *m, const uint8_t *data, long size, const char *name);
int st3_patch_help(Machine *m);   /* st3patch.c: Ctrl-F line in ST3's global-keys quickhelp; 0 = applied */
/* run a slice of n instructions (handles HLE traps, IRQ delivery, timers). returns <0 on fatal error */
int machine_run(Machine *m, uint64_t n);
double machine_vtime(Machine *m);
void machine_key(Machine *m, uint16_t key);
int machine_keyq_free(Machine *m);
void machine_dump_vga_state(Machine *m, const char *json_path, const char *font_path);

/* io.c */
uint32_t io_in(void *ud, uint16_t port, int size);
void io_out(void *ud, uint16_t port, int size, uint32_t val);
void timers_update(Machine *m);
void sb_update(Machine *m);
/* mixer.c */
#define MIX_RATE 48000
void mixer_dac(Machine *m, const uint8_t *d, int n, double rate, int stereo);
void mixer_update(Machine *m);
/* vga.c */
void vga_init(Machine *m);
uint8_t vga_in(Machine *m, uint16_t port);
void vga_out(Machine *m, uint16_t port, uint8_t v);
/* bios.c / dos.c */
void bios_init(Machine *m);
int bios_service(Machine *m, int vec, uint16_t *flags);   /* returns 1 if handled */
int dos_service(Machine *m, uint16_t *flags);
void dos_init(Machine *m);
uint16_t dos_alloc(Machine *m, uint16_t paras, uint16_t *maxfree);

#define LOGF(m, ...) do { if ((m)->log) { fprintf((m)->log, __VA_ARGS__); fflush((m)->log); } } while (0)
#endif
