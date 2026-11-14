/* cpu.h — 386 real-mode integer interpreter for the ST3 port. */
#ifndef ST3_CPU_H
#define ST3_CPU_H
#include <stdint.h>

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

#define MEM_SIZE (0x110000 + 16)   /* 1MB + HMA + slack for unaligned reads */

typedef struct CPU CPU;
typedef uint32_t (*io_in_fn)(void *ud, uint16_t port, int size);
typedef void (*io_out_fn)(void *ud, uint16_t port, int size, uint32_t val);

struct CPU {
    uint32_t r[8];
    uint16_t seg[6];
    uint32_t eip;
    uint32_t flags;
    uint8_t *mem;            /* MEM_SIZE bytes */
    uint32_t a20_mask;       /* 0xFFFFF (A20 off) or 0x1FFFFF */
    io_in_fn io_in;
    io_out_fn io_out;
    void *ud;
    uint64_t icount;         /* instructions executed */
    int halted;
    int error;               /* nonzero: unimplemented opcode etc. */
    uint32_t hist[256][2];   /* last 256 non-sequential transfers: from CS:IP, to CS:IP (ring, for crash reports) */
    uint8_t hpos;
    uint32_t hprev, hprevlin;
    uint32_t err_lin;        /* linear address of faulting instruction */
    uint8_t err_bytes[8];
    int inhibit_irq;         /* one-instruction interrupt shadow (after mov ss / pop ss / sti) */
};

void cpu_init(CPU *c, uint8_t *mem);
/* Execute one instruction. Returns 0 ok, nonzero on fatal error (c->error set). */
int cpu_step(CPU *c);
/* Run up to n instructions or until stop_lin hit (checked before each instruction; 0 = none). */
int cpu_run(CPU *c, uint64_t n, uint32_t trap_lo, uint32_t trap_hi, uint32_t *trapped);
/* Software-style interrupt delivery (used for hardware IRQs and HLE). */
void cpu_interrupt(CPU *c, int vec);
static inline uint32_t cpu_lin(CPU *c) { return ((uint32_t)c->seg[S_CS] << 4) + (c->eip & 0xFFFF); }

/* stack helpers usable by HLE */
void cpu_push16(CPU *c, uint16_t v);
uint16_t cpu_pop16(CPU *c);

#endif
