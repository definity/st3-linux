/* cpu.c — 386 real-mode integer interpreter (no FPU, no protected mode).
 * Written for the Scream Tracker 3.21 port; verified by differential fuzzing against Unicorn (tests/). */
#include "cpu.h"
#include <string.h>

/* ------------------------------------------------------------------ memory */
static inline uint8_t rd8(CPU *c, uint32_t a) { return c->mem[a & c->a20_mask]; }
static inline void wr8(CPU *c, uint32_t a, uint8_t v) { c->mem[a & c->a20_mask] = v; }
static inline uint16_t rd16(CPU *c, uint32_t a) { return rd8(c, a) | (rd8(c, a + 1) << 8); }
static inline void wr16(CPU *c, uint32_t a, uint16_t v) { wr8(c, a, v); wr8(c, a + 1, v >> 8); }
static inline uint32_t rd32(CPU *c, uint32_t a) { return rd16(c, a) | ((uint32_t)rd16(c, a + 2) << 16); }
static inline void wr32(CPU *c, uint32_t a, uint32_t v) { wr16(c, a, v); wr16(c, a + 2, v >> 16); }
static inline uint32_t rdw(CPU *c, uint32_t a, int w) { return w == 1 ? rd8(c, a) : w == 2 ? rd16(c, a) : rd32(c, a); }
static inline void wrw(CPU *c, uint32_t a, uint32_t v, int w) {
    if (w == 1) wr8(c, a, v); else if (w == 2) wr16(c, a, v); else wr32(c, a, v);
}
static inline uint32_t lin(CPU *c, int s, uint32_t off) { return ((uint32_t)c->seg[s] << 4) + off; }

/* ------------------------------------------------------------------ helpers */
static const uint32_t MASK[5] = {0, 0xFF, 0xFFFF, 0, 0xFFFFFFFFu};
static const uint32_t SIGN[5] = {0, 0x80, 0x8000, 0, 0x80000000u};
static uint8_t parity[256];

#define FL c->flags
#define SETF(f, v) do { if (v) FL |= (f); else FL &= ~(uint32_t)(f); } while (0)
#define GETF(f) ((FL & (f)) != 0)

static inline void szp(CPU *c, uint32_t r, int w) {
    r &= MASK[w];
    SETF(F_ZF, r == 0);
    SETF(F_SF, r & SIGN[w]);
    SETF(F_PF, parity[r & 0xFF]);
}

static inline uint32_t getreg(CPU *c, int w, int i) {
    if (w == 1) return i < 4 ? (c->r[i] & 0xFF) : ((c->r[i - 4] >> 8) & 0xFF);
    if (w == 2) return c->r[i] & 0xFFFF;
    return c->r[i];
}
static inline void setreg(CPU *c, int w, int i, uint32_t v) {
    if (w == 1) {
        if (i < 4) c->r[i] = (c->r[i] & ~0xFFu) | (v & 0xFF);
        else c->r[i - 4] = (c->r[i - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
    } else if (w == 2) c->r[i] = (c->r[i] & 0xFFFF0000u) | (v & 0xFFFF);
    else c->r[i] = v;
}

/* ------------------------------------------------------------------ decode state */
typedef struct {
    int seg;        /* segment override, -1 = none */
    int osz;        /* operand size 32? */
    int asz;        /* address size 32? */
    int rep;        /* 0, 0xF2, 0xF3 */
    uint16_t start_ip;
    /* modrm */
    int mod, reg, rm;
    uint32_t ea;    /* effective offset */
    int eseg;       /* effective segment */
} Dec;

static inline uint8_t fetch8(CPU *c) {
    uint8_t v = rd8(c, lin(c, S_CS, c->eip & 0xFFFF));
    c->eip = (c->eip + 1) & 0xFFFF;
    return v;
}
static inline uint16_t fetch16(CPU *c) { uint16_t v = fetch8(c); return v | (fetch8(c) << 8); }
static inline uint32_t fetch32(CPU *c) { uint32_t v = fetch16(c); return v | ((uint32_t)fetch16(c) << 16); }

static void modrm(CPU *c, Dec *d) {
    uint8_t m = fetch8(c);
    d->mod = m >> 6; d->reg = (m >> 3) & 7; d->rm = m & 7;
    if (d->mod == 3) return;
    int defseg = S_DS;
    uint32_t ea = 0;
    if (!d->asz) {
        switch (d->rm) {
        case 0: ea = c->r[R_EBX] + c->r[R_ESI]; break;
        case 1: ea = c->r[R_EBX] + c->r[R_EDI]; break;
        case 2: ea = c->r[R_EBP] + c->r[R_ESI]; defseg = S_SS; break;
        case 3: ea = c->r[R_EBP] + c->r[R_EDI]; defseg = S_SS; break;
        case 4: ea = c->r[R_ESI]; break;
        case 5: ea = c->r[R_EDI]; break;
        case 6: if (d->mod == 0) ea = fetch16(c); else { ea = c->r[R_EBP]; defseg = S_SS; } break;
        case 7: ea = c->r[R_EBX]; break;
        }
        if (d->mod == 1) ea += (int8_t)fetch8(c);
        else if (d->mod == 2) ea += fetch16(c);
        ea &= 0xFFFF;
    } else {
        int rm = d->rm;
        if (rm == 4) {
            uint8_t sib = fetch8(c);
            int ss = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (base == 5 && d->mod == 0) ea = fetch32(c);
            else { ea = c->r[base]; if (base == R_ESP || base == R_EBP) defseg = S_SS; }
            if (idx != 4) ea += c->r[idx] << ss;
        } else if (rm == 5 && d->mod == 0) {
            ea = fetch32(c);
        } else {
            ea = c->r[rm];
            if (rm == R_EBP) defseg = S_SS;
        }
        if (d->mod == 1) ea += (int8_t)fetch8(c);
        else if (d->mod == 2) ea += fetch32(c);
    }
    d->ea = ea;
    d->eseg = d->seg >= 0 ? d->seg : defseg;
}

static inline uint32_t ealin(CPU *c, Dec *d, uint32_t add) {
    uint32_t off = d->ea + add;
    if (!d->asz) off &= 0xFFFF;
    return lin(c, d->eseg, off);
}
static inline uint32_t rm_read(CPU *c, Dec *d, int w) {
    return d->mod == 3 ? getreg(c, w, d->rm) : rdw(c, ealin(c, d, 0), w);
}
static inline void rm_write(CPU *c, Dec *d, int w, uint32_t v) {
    if (d->mod == 3) setreg(c, w, d->rm, v); else wrw(c, ealin(c, d, 0), v, w);
}

/* ------------------------------------------------------------------ stack */
static inline void push(CPU *c, uint32_t v, int w) {
    uint16_t sp = (c->r[R_ESP] - w) & 0xFFFF;
    c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000u) | sp;
    wrw(c, lin(c, S_SS, sp), v, w);
}
static inline uint32_t pop(CPU *c, int w) {
    uint16_t sp = c->r[R_ESP] & 0xFFFF;
    uint32_t v = rdw(c, lin(c, S_SS, sp), w);
    c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000u) | ((sp + w) & 0xFFFF);
    return v;
}
void cpu_push16(CPU *c, uint16_t v) { push(c, v, 2); }
uint16_t cpu_pop16(CPU *c) { return pop(c, 2); }

/* ------------------------------------------------------------------ ALU */
static uint32_t alu(CPU *c, int op, uint32_t a, uint32_t b, int w) {
    uint32_t m = MASK[w], s = SIGN[w], r = 0;
    uint64_t wide;
    a &= m; b &= m;
    switch (op) {
    case 0: /* ADD */
        wide = (uint64_t)a + b; r = wide & m;
        SETF(F_CF, wide > m); SETF(F_OF, (a ^ r) & (b ^ r) & s); SETF(F_AF, (a ^ b ^ r) & 0x10); break;
    case 2: { /* ADC */
        uint32_t cf = GETF(F_CF);
        wide = (uint64_t)a + b + cf; r = wide & m;
        SETF(F_CF, wide > m); SETF(F_OF, (a ^ r) & (b ^ r) & s); SETF(F_AF, (a ^ b ^ r) & 0x10); break; }
    case 3: { /* SBB */
        uint32_t cf = GETF(F_CF);
        r = (a - b - cf) & m;
        SETF(F_CF, (uint64_t)b + cf > a); SETF(F_OF, (a ^ b) & (a ^ r) & s); SETF(F_AF, (a ^ b ^ r) & 0x10); break; }
    case 5: case 7: /* SUB, CMP */
        r = (a - b) & m;
        SETF(F_CF, a < b); SETF(F_OF, (a ^ b) & (a ^ r) & s); SETF(F_AF, (a ^ b ^ r) & 0x10); break;
    case 1: r = a | b; goto logic;
    case 4: r = a & b; goto logic;
    case 6: r = a ^ b;
    logic:
        FL &= ~(uint32_t)(F_CF | F_OF | F_AF); break;
    }
    szp(c, r, w);
    return op == 7 ? a : r;
}

static uint32_t incdec(CPU *c, uint32_t a, int w, int dec) {
    uint32_t m = MASK[w], s = SIGN[w], r;
    a &= m;
    if (!dec) { r = (a + 1) & m; SETF(F_OF, r == s); SETF(F_AF, (r & 0xF) == 0); }
    else { r = (a - 1) & m; SETF(F_OF, a == s); SETF(F_AF, (a & 0xF) == 0); }
    szp(c, r, w);
    return r;
}

static uint32_t shiftop(CPU *c, int op, uint32_t a, unsigned cnt, int w) {
    uint32_t m = MASK[w], s = SIGN[w];
    int n = w * 8;
    cnt &= 31;
    if (!cnt) return a & m;
    a &= m;
    uint32_t r = a;
    switch (op) {
    case 0: { /* ROL */
        unsigned k = cnt % n;
        r = k ? ((a << k) | (a >> (n - k))) & m : a;
        SETF(F_CF, r & 1); SETF(F_OF, ((r & s) != 0) ^ (r & 1)); return r; }
    case 1: { /* ROR */
        unsigned k = cnt % n;
        r = k ? ((a >> k) | (a << (n - k))) & m : a;
        SETF(F_CF, r & s); SETF(F_OF, ((r ^ (r << 1)) & s) != 0); return r; }
    case 2: { /* RCL */
        unsigned k = cnt % (n + 1);
        uint32_t cf = GETF(F_CF);
        for (unsigned i = 0; i < k; i++) { uint32_t ncf = (r & s) != 0; r = ((r << 1) | cf) & m; cf = ncf; }
        SETF(F_CF, cf); SETF(F_OF, ((r & s) != 0) ^ cf); return r; }
    case 3: { /* RCR */
        unsigned k = cnt % (n + 1);
        uint32_t cf = GETF(F_CF);
        SETF(F_OF, ((a & s) != 0) ^ cf);
        for (unsigned i = 0; i < k; i++) { uint32_t ncf = r & 1; r = (r >> 1) | (cf ? s : 0); cf = ncf; }
        SETF(F_CF, cf); return r; }
    case 4: case 6: { /* SHL/SAL */
        uint64_t wide = (uint64_t)a << cnt;
        r = wide & m;
        SETF(F_CF, (wide >> n) & 1);
        SETF(F_OF, ((r & s) != 0) ^ GETF(F_CF));
        FL &= ~(uint32_t)F_AF;
        szp(c, r, w); return r; }
    case 5: /* SHR */
        SETF(F_CF, cnt <= 32 ? (((uint64_t)a >> (cnt - 1)) & 1) : 0);
        r = (uint32_t)(((uint64_t)a >> cnt) & m);
        SETF(F_OF, a & s);
        FL &= ~(uint32_t)F_AF;
        szp(c, r, w); return r;
    case 7: { /* SAR */
        int64_t sa = (w == 1) ? (int8_t)a : (w == 2) ? (int16_t)a : (int32_t)a;
        unsigned k = cnt >= (unsigned)n ? (unsigned)n - 1 : cnt;
        SETF(F_CF, cnt >= (unsigned)n ? (sa < 0) : ((sa >> (cnt - 1)) & 1));
        r = (uint32_t)((sa >> k) & m);
        if (cnt >= (unsigned)n) r = sa < 0 ? m : 0;
        FL &= ~(uint32_t)(F_OF | F_AF);
        szp(c, r, w); return r; }
    }
    return r;
}

static int cond(CPU *c, int n) {
    int r;
    switch (n >> 1) {
    case 0: r = GETF(F_OF); break;
    case 1: r = GETF(F_CF); break;
    case 2: r = GETF(F_ZF); break;
    case 3: r = GETF(F_CF) || GETF(F_ZF); break;
    case 4: r = GETF(F_SF); break;
    case 5: r = GETF(F_PF); break;
    case 6: r = GETF(F_SF) != GETF(F_OF); break;
    default: r = GETF(F_ZF) || (GETF(F_SF) != GETF(F_OF)); break;
    }
    return (n & 1) ? !r : r;
}

/* ------------------------------------------------------------------ interrupts */
void cpu_interrupt(CPU *c, int vec) {
    push(c, (FL & 0xFFFF) | 2, 2);
    push(c, c->seg[S_CS], 2);
    push(c, c->eip & 0xFFFF, 2);
    FL &= ~(uint32_t)(F_IF | F_TF);
    c->eip = rd16(c, vec * 4);
    c->seg[S_CS] = rd16(c, vec * 4 + 2);
    c->halted = 0;
}

static void fault(CPU *c, Dec *d, int vec) {   /* faults restart at the faulting instruction */
    c->eip = d->start_ip;
    cpu_interrupt(c, vec);
}

static int unimpl(CPU *c, Dec *d) {
    c->error = 1;
    c->err_lin = lin(c, S_CS, d->start_ip);
    for (int i = 0; i < 8; i++) c->err_bytes[i] = rd8(c, c->err_lin + i);
    c->eip = d->start_ip;
    return -1;
}

static inline void load_flags(CPU *c, uint32_t v, int w) {
    if (w == 2) FL = (FL & 0xFFFF0000u) | (v & 0x7FD5) | 2;
    else FL = (v & 0x00037FD5) | 2;   /* VM/RF not settable in real mode via popfd here */
}

/* ------------------------------------------------------------------ string ops */
static inline uint32_t sreg_get(CPU *c, Dec *d, int r) { return d->asz ? c->r[r] : (c->r[r] & 0xFFFF); }
static inline void sreg_add(CPU *c, Dec *d, int r, int32_t delta) {
    if (d->asz) c->r[r] += delta;
    else c->r[r] = (c->r[r] & 0xFFFF0000u) | ((c->r[r] + delta) & 0xFFFF);
}
static inline uint32_t cnt_get(CPU *c, Dec *d) { return d->asz ? c->r[R_ECX] : (c->r[R_ECX] & 0xFFFF); }
static inline void cnt_dec(CPU *c, Dec *d) { sreg_add(c, d, R_ECX, -1); }

static void string_op(CPU *c, Dec *d, uint8_t op) {
    int w = (op & 1) ? (d->osz ? 4 : 2) : 1;
    int32_t delta = GETF(F_DF) ? -w : w;
    int srcseg = d->seg >= 0 ? d->seg : S_DS;
    for (;;) {
        if (d->rep && cnt_get(c, d) == 0) break;
        switch (op & 0xFE) {
        case 0xA4: { uint32_t v = rdw(c, lin(c, srcseg, sreg_get(c, d, R_ESI)), w);
            wrw(c, lin(c, S_ES, sreg_get(c, d, R_EDI)), v, w);
            sreg_add(c, d, R_ESI, delta); sreg_add(c, d, R_EDI, delta); break; }
        case 0xA6: { uint32_t a = rdw(c, lin(c, srcseg, sreg_get(c, d, R_ESI)), w);
            uint32_t b = rdw(c, lin(c, S_ES, sreg_get(c, d, R_EDI)), w);
            alu(c, 7, a, b, w); sreg_add(c, d, R_ESI, delta); sreg_add(c, d, R_EDI, delta); break; }
        case 0xAA: wrw(c, lin(c, S_ES, sreg_get(c, d, R_EDI)), getreg(c, w, R_EAX), w);
            sreg_add(c, d, R_EDI, delta); break;
        case 0xAC: setreg(c, w, R_EAX, rdw(c, lin(c, srcseg, sreg_get(c, d, R_ESI)), w));
            sreg_add(c, d, R_ESI, delta); break;
        case 0xAE: { uint32_t b = rdw(c, lin(c, S_ES, sreg_get(c, d, R_EDI)), w);
            alu(c, 7, getreg(c, w, R_EAX), b, w); sreg_add(c, d, R_EDI, delta); break; }
        case 0x6C: wrw(c, lin(c, S_ES, sreg_get(c, d, R_EDI)), c->io_in(c->ud, c->r[R_EDX] & 0xFFFF, w), w);
            sreg_add(c, d, R_EDI, delta); break;
        case 0x6E: c->io_out(c->ud, c->r[R_EDX] & 0xFFFF, w, rdw(c, lin(c, srcseg, sreg_get(c, d, R_ESI)), w));
            sreg_add(c, d, R_ESI, delta); break;
        }
        if (!d->rep) break;
        cnt_dec(c, d);
        if ((op & 0xF6) == 0xA6) {   /* CMPS/SCAS: A6,A7,AE,AF */
            if (d->rep == 0xF3 && !GETF(F_ZF)) break;
            if (d->rep == 0xF2 && GETF(F_ZF)) break;
        }
    }
}

/* ------------------------------------------------------------------ mul/div */
static int grp3(CPU *c, Dec *d, int w) {
    uint32_t v = rm_read(c, d, w);
    switch (d->reg) {
    case 0: case 1: { uint32_t imm = w == 1 ? fetch8(c) : w == 2 ? fetch16(c) : fetch32(c);
        alu(c, 4, v, imm, w); return 0; }
    case 2: rm_write(c, d, w, ~v & MASK[w]); return 0;
    case 3: { uint32_t r = alu(c, 5, 0, v, w); SETF(F_CF, (v & MASK[w]) != 0); rm_write(c, d, w, r); return 0; }
    case 4: /* MUL */
        if (w == 1) { uint32_t r = (c->r[R_EAX] & 0xFF) * v; setreg(c, 2, R_EAX, r); SETF(F_CF, r >> 8); SETF(F_OF, r >> 8); szp(c, r, 1); }
        else if (w == 2) { uint32_t r = (c->r[R_EAX] & 0xFFFF) * v; setreg(c, 2, R_EAX, r); setreg(c, 2, R_EDX, r >> 16);
            SETF(F_CF, r >> 16); SETF(F_OF, r >> 16); szp(c, r, 2); }
        else { uint64_t r = (uint64_t)c->r[R_EAX] * v; c->r[R_EAX] = (uint32_t)r; c->r[R_EDX] = r >> 32;
            SETF(F_CF, r >> 32); SETF(F_OF, r >> 32); szp(c, (uint32_t)r, 4); }
        return 0;
    case 5: /* IMUL */
        if (w == 1) { int32_t r = (int8_t)c->r[R_EAX] * (int8_t)v; setreg(c, 2, R_EAX, r);
            int ov = r != (int8_t)r; SETF(F_CF, ov); SETF(F_OF, ov); szp(c, r, 1); }
        else if (w == 2) { int32_t r = (int16_t)c->r[R_EAX] * (int16_t)v; setreg(c, 2, R_EAX, r); setreg(c, 2, R_EDX, r >> 16);
            int ov = r != (int16_t)r; SETF(F_CF, ov); SETF(F_OF, ov); szp(c, r, 2); }
        else { int64_t r = (int64_t)(int32_t)c->r[R_EAX] * (int32_t)v; c->r[R_EAX] = (uint32_t)r; c->r[R_EDX] = (uint64_t)r >> 32;
            int ov = r != (int32_t)r; SETF(F_CF, ov); SETF(F_OF, ov); szp(c, (uint32_t)r, 4); }
        return 0;
    case 6: /* DIV */
        if (w == 1) { uint32_t a = c->r[R_EAX] & 0xFFFF;
            if (!v || a / v > 0xFF) { fault(c, d, 0); return 0; }
            setreg(c, 1, 0, a / v); setreg(c, 1, 4, a % v); }
        else if (w == 2) { uint32_t a = ((c->r[R_EDX] & 0xFFFF) << 16) | (c->r[R_EAX] & 0xFFFF);
            if (!v || a / v > 0xFFFF) { fault(c, d, 0); return 0; }
            setreg(c, 2, R_EAX, a / v); setreg(c, 2, R_EDX, a % v); }
        else { uint64_t a = ((uint64_t)c->r[R_EDX] << 32) | c->r[R_EAX];
            if (!v || a / v > 0xFFFFFFFFull) { fault(c, d, 0); return 0; }
            c->r[R_EAX] = a / v; c->r[R_EDX] = a % v; }
        return 0;
    case 7: /* IDIV */
        if (w == 1) { int32_t a = (int16_t)c->r[R_EAX], b = (int8_t)v;
            if (!b) { fault(c, d, 0); return 0; }
            int32_t q = a / b; if (q > 127 || q < -128) { fault(c, d, 0); return 0; }
            setreg(c, 1, 0, q); setreg(c, 1, 4, a % b); }
        else if (w == 2) { int32_t a = (int32_t)(((c->r[R_EDX] & 0xFFFF) << 16) | (c->r[R_EAX] & 0xFFFF)), b = (int16_t)v;
            if (!b) { fault(c, d, 0); return 0; }
            int32_t q = (a == INT32_MIN && b == -1) ? 0x10000 : a / b;
            if (q > 32767 || q < -32768) { fault(c, d, 0); return 0; }
            setreg(c, 2, R_EAX, q); setreg(c, 2, R_EDX, a % b); }
        else { int64_t a = (int64_t)(((uint64_t)c->r[R_EDX] << 32) | c->r[R_EAX]), b = (int32_t)v;
            if (!b) { fault(c, d, 0); return 0; }
            if (a == INT64_MIN && b == -1) { fault(c, d, 0); return 0; }
            int64_t q = a / b; if (q > INT32_MAX || q < INT32_MIN) { fault(c, d, 0); return 0; }
            c->r[R_EAX] = (uint32_t)q; c->r[R_EDX] = (uint32_t)(a % b); }
        return 0;
    }
    return 0;
}

static uint32_t imul3(CPU *c, uint32_t a, uint32_t b, int w) {
    int64_t r;
    if (w == 2) r = (int64_t)(int16_t)a * (int16_t)b;
    else r = (int64_t)(int32_t)a * (int32_t)b;
    int ov = (w == 2) ? r != (int16_t)r : r != (int32_t)r;
    SETF(F_CF, ov); SETF(F_OF, ov); szp(c, (uint32_t)r, w);
    return (uint32_t)r & MASK[w];
}

/* ------------------------------------------------------------------ 0F map */
static int op0f(CPU *c, Dec *d) {
    uint8_t op = fetch8(c);
    int w = d->osz ? 4 : 2;
    if (op >= 0x80 && op <= 0x8F) {
        int32_t rel = d->osz ? (int32_t)fetch32(c) : (int16_t)fetch16(c);
        if (cond(c, op & 15)) c->eip = (c->eip + rel) & 0xFFFF;
        return 0;
    }
    if (op >= 0x90 && op <= 0x9F) { modrm(c, d); rm_write(c, d, 1, cond(c, op & 15)); return 0; }
    switch (op) {
    case 0x01: modrm(c, d);
        if (d->reg == 4) { rm_write(c, d, 2, 0x0000); return 0; }   /* SMSW: real mode, PE=0 */
        return unimpl(c, d);
    case 0x06: return 0;   /* CLTS */
    case 0xA0: push(c, c->seg[S_FS], w); return 0;
    case 0xA1: c->seg[S_FS] = pop(c, w); return 0;
    case 0xA8: push(c, c->seg[S_GS], w); return 0;
    case 0xA9: c->seg[S_GS] = pop(c, w); return 0;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: {
        modrm(c, d);
        uint32_t bit; int kind;
        if (op == 0xBA) { bit = fetch8(c) & (w * 8 - 1); kind = d->reg - 4; if (kind < 0) return unimpl(c, d); }
        else {
            kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
            uint32_t off = getreg(c, w, d->reg);
            if (d->mod != 3) {
                int32_t soff = (w == 2) ? (int16_t)off : (int32_t)off;
                int32_t byteoff = (soff >> (w == 2 ? 4 : 5)) * w;
                d->ea += byteoff;
            }
            bit = off & (w * 8 - 1);
        }
        uint32_t v = rm_read(c, d, w);
        SETF(F_CF, (v >> bit) & 1);
        if (kind == 1) rm_write(c, d, w, v | (1u << bit));
        else if (kind == 2) rm_write(c, d, w, v & ~(1u << bit));
        else if (kind == 3) rm_write(c, d, w, v ^ (1u << bit));
        return 0; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
        modrm(c, d);
        unsigned cnt = ((op & 1) ? (c->r[R_ECX] & 0xFF) : fetch8(c)) & 31;
        if (!cnt) return 0;
        uint32_t a = rm_read(c, d, w), b = getreg(c, w, d->reg), r;
        int n = w * 8;
        if (op <= 0xA5) {   /* SHLD */
            uint64_t x = ((uint64_t)a << n) | b;
            if (w == 2) x = (x << 16) | b;   /* 16-bit: pattern a:b:b for counts > 16 (matches Intel/QEMU) */
            uint64_t sh = x << cnt;
            if (w == 2) { r = (uint32_t)(sh >> 32) & 0xFFFF; SETF(F_CF, (sh >> 48) & 1); }
            else { r = (uint32_t)(sh >> 32); SETF(F_CF, (x >> (64 - cnt)) & 1); }
        } else {            /* SHRD */
            uint64_t x = ((uint64_t)b << n) | a;
            if (w == 2) x = ((uint64_t)b << 32) | x;
            r = (uint32_t)(x >> cnt) & MASK[w];
            SETF(F_CF, (x >> (cnt - 1)) & 1);
        }
        SETF(F_OF, ((r ^ a) & SIGN[w]) != 0);
        szp(c, r, w); rm_write(c, d, w, r); return 0; }
    case 0xAF: modrm(c, d); setreg(c, w, d->reg, imul3(c, getreg(c, w, d->reg), rm_read(c, d, w), w)); return 0;
    case 0xB2: case 0xB4: case 0xB5: {
        modrm(c, d); if (d->mod == 3) return unimpl(c, d);
        uint32_t off = rdw(c, ealin(c, d, 0), w); uint16_t sg = rd16(c, ealin(c, d, w));
        setreg(c, w, d->reg, off);
        c->seg[op == 0xB2 ? S_SS : op == 0xB4 ? S_FS : S_GS] = sg;
        if (op == 0xB2) c->inhibit_irq = 1;
        return 0; }
    case 0xB6: modrm(c, d); setreg(c, w, d->reg, rm_read(c, d, 1)); return 0;
    case 0xB7: modrm(c, d); setreg(c, w, d->reg, rm_read(c, d, 2)); return 0;
    case 0xBE: modrm(c, d); setreg(c, w, d->reg, (uint32_t)(int32_t)(int8_t)rm_read(c, d, 1)); return 0;
    case 0xBF: modrm(c, d); setreg(c, w, d->reg, (uint32_t)(int32_t)(int16_t)rm_read(c, d, 2)); return 0;
    case 0xBC: case 0xBD: {
        modrm(c, d); uint32_t v = rm_read(c, d, w);
        if (!v) { FL |= F_ZF; return 0; }
        FL &= ~(uint32_t)F_ZF;
        int i;
        if (op == 0xBC) { for (i = 0; !((v >> i) & 1); i++); }
        else { for (i = w * 8 - 1; !((v >> i) & 1); i--); }
        setreg(c, w, d->reg, i); return 0; }
    }
    return unimpl(c, d);
}

/* ------------------------------------------------------------------ main step */
int cpu_step(CPU *c) {
    Dec dd, *d = &dd;
    d->seg = -1; d->osz = 0; d->asz = 0; d->rep = 0;
    d->start_ip = c->eip & 0xFFFF;
    c->inhibit_irq = 0;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        switch (op) {
        case 0x26: d->seg = S_ES; continue;
        case 0x2E: d->seg = S_CS; continue;
        case 0x36: d->seg = S_SS; continue;
        case 0x3E: d->seg = S_DS; continue;
        case 0x64: d->seg = S_FS; continue;
        case 0x65: d->seg = S_GS; continue;
        case 0x66: d->osz = 1; continue;
        case 0x67: d->asz = 1; continue;
        case 0xF0: continue;
        case 0xF2: case 0xF3: d->rep = op; continue;
        }
        break;
    }
    c->icount++;
    int w = d->osz ? 4 : 2;

    if (op < 0x40 && (op & 7) < 6) {   /* ALU block */
        int aop = op >> 3, form = op & 7;
        switch (form) {
        case 0: modrm(c, d); { uint32_t r = alu(c, aop, rm_read(c, d, 1), getreg(c, 1, d->reg), 1); if (aop != 7) rm_write(c, d, 1, r); } break;
        case 1: modrm(c, d); { uint32_t r = alu(c, aop, rm_read(c, d, w), getreg(c, w, d->reg), w); if (aop != 7) rm_write(c, d, w, r); } break;
        case 2: modrm(c, d); { uint32_t r = alu(c, aop, getreg(c, 1, d->reg), rm_read(c, d, 1), 1); if (aop != 7) setreg(c, 1, d->reg, r); } break;
        case 3: modrm(c, d); { uint32_t r = alu(c, aop, getreg(c, w, d->reg), rm_read(c, d, w), w); if (aop != 7) setreg(c, w, d->reg, r); } break;
        case 4: { uint32_t r = alu(c, aop, getreg(c, 1, 0), fetch8(c), 1); if (aop != 7) setreg(c, 1, 0, r); } break;
        case 5: { uint32_t imm = d->osz ? fetch32(c) : fetch16(c); uint32_t r = alu(c, aop, getreg(c, w, 0), imm, w); if (aop != 7) setreg(c, w, 0, r); } break;
        }
        return 0;
    }

    switch (op) {
    case 0x06: push(c, c->seg[S_ES], w); break;
    case 0x07: c->seg[S_ES] = pop(c, w); break;
    case 0x0E: push(c, c->seg[S_CS], w); break;
    case 0x0F: return op0f(c, d);
    case 0x16: push(c, c->seg[S_SS], w); break;
    case 0x17: c->seg[S_SS] = pop(c, w); c->inhibit_irq = 1; break;
    case 0x1E: push(c, c->seg[S_DS], w); break;
    case 0x1F: c->seg[S_DS] = pop(c, w); break;
    case 0x27: case 0x2F: {   /* DAA / DAS */
        uint8_t al = c->r[R_EAX] & 0xFF, old = al; int ocf = GETF(F_CF), cf = 0;
        if ((al & 0xF) > 9 || GETF(F_AF)) {
            if (op == 0x27) { cf = ocf || al > 0xF9; al += 6; } else { cf = ocf || al < 6; al -= 6; }
            FL |= F_AF;
        } else FL &= ~(uint32_t)F_AF;
        if (old > 0x99 || ocf) { al = op == 0x27 ? al + 0x60 : al - 0x60; cf = 1; }
        SETF(F_CF, cf); setreg(c, 1, 0, al); szp(c, al, 1); break; }
    case 0x37: case 0x3F: {   /* AAA / AAS */
        if ((c->r[R_EAX] & 0xF) > 9 || GETF(F_AF)) {
            uint16_t ax = c->r[R_EAX] & 0xFFFF;
            if (op == 0x37) ax = ((ax + 0x106) & 0xFF00) | ((ax + 6) & 0xFF);
            else ax = ((ax - 0x100) & 0xFF00) | ((ax - 6) & 0xFF);
            setreg(c, 2, R_EAX, ax); FL |= F_AF | F_CF;
        } else FL &= ~(uint32_t)(F_AF | F_CF);
        setreg(c, 1, 0, c->r[R_EAX] & 0x0F); break; }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        setreg(c, w, op & 7, incdec(c, getreg(c, w, op & 7), w, 0)); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        setreg(c, w, op & 7, incdec(c, getreg(c, w, op & 7), w, 1)); break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        uint32_t v = getreg(c, w, op & 7);   /* 286+: PUSH SP pushes old SP */
        push(c, v, w); break; }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        { uint32_t v = pop(c, w); setreg(c, w, op & 7, v); } break;
    case 0x60: { uint32_t sp = getreg(c, w, R_ESP);
        for (int i = 0; i < 8; i++) push(c, i == R_ESP ? sp : getreg(c, w, i), w); break; }
    case 0x61: for (int i = 7; i >= 0; i--) { uint32_t v = pop(c, w); if (i != R_ESP) setreg(c, w, i, v); } break;
    case 0x62: modrm(c, d); break;   /* BOUND: assume in range */
    case 0x68: push(c, d->osz ? fetch32(c) : fetch16(c), w); break;
    case 0x6A: push(c, (uint32_t)(int32_t)(int8_t)fetch8(c), w); break;
    case 0x69: case 0x6B: { modrm(c, d); uint32_t a = rm_read(c, d, w);
        uint32_t b = op == 0x6B ? (uint32_t)(int32_t)(int8_t)fetch8(c) : (d->osz ? fetch32(c) : (uint32_t)(int32_t)(int16_t)fetch16(c));
        setreg(c, w, d->reg, imul3(c, a, b, w)); break; }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, d, op); break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = fetch8(c); if (cond(c, op & 15)) c->eip = (c->eip + rel) & 0xFFFF; break; }
    case 0x80: case 0x82: { modrm(c, d); uint32_t a = rm_read(c, d, 1); uint32_t r = alu(c, d->reg, a, fetch8(c), 1);
        if (d->reg != 7) rm_write(c, d, 1, r); break; }
    case 0x81: { modrm(c, d); uint32_t a = rm_read(c, d, w); uint32_t imm = d->osz ? fetch32(c) : fetch16(c);
        uint32_t r = alu(c, d->reg, a, imm, w); if (d->reg != 7) rm_write(c, d, w, r); break; }
    case 0x83: { modrm(c, d); uint32_t a = rm_read(c, d, w); uint32_t imm = (uint32_t)(int32_t)(int8_t)fetch8(c);
        uint32_t r = alu(c, d->reg, a, imm, w); if (d->reg != 7) rm_write(c, d, w, r); break; }
    case 0x84: modrm(c, d); alu(c, 4, rm_read(c, d, 1), getreg(c, 1, d->reg), 1); break;
    case 0x85: modrm(c, d); alu(c, 4, rm_read(c, d, w), getreg(c, w, d->reg), w); break;
    case 0x86: { modrm(c, d); uint32_t a = rm_read(c, d, 1); rm_write(c, d, 1, getreg(c, 1, d->reg)); setreg(c, 1, d->reg, a); break; }
    case 0x87: { modrm(c, d); uint32_t a = rm_read(c, d, w); rm_write(c, d, w, getreg(c, w, d->reg)); setreg(c, w, d->reg, a); break; }
    case 0x88: modrm(c, d); rm_write(c, d, 1, getreg(c, 1, d->reg)); break;
    case 0x89: modrm(c, d); rm_write(c, d, w, getreg(c, w, d->reg)); break;
    case 0x8A: modrm(c, d); setreg(c, 1, d->reg, rm_read(c, d, 1)); break;
    case 0x8B: modrm(c, d); setreg(c, w, d->reg, rm_read(c, d, w)); break;
    case 0x8C: modrm(c, d); if (d->reg > 5) return unimpl(c, d);
        if (d->mod == 3) setreg(c, w, d->rm, c->seg[d->reg]); else wr16(c, ealin(c, d, 0), c->seg[d->reg]); break;
    case 0x8D: modrm(c, d); if (d->mod == 3) return unimpl(c, d); setreg(c, w, d->reg, d->ea); break;
    case 0x8E: modrm(c, d); if (d->reg > 5 || d->reg == S_CS) return unimpl(c, d);
        c->seg[d->reg] = rm_read(c, d, 2); if (d->reg == S_SS) c->inhibit_irq = 1; break;
    case 0x8F: { uint32_t v = pop(c, w); modrm(c, d); rm_write(c, d, w, v); break; }
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint32_t a = getreg(c, w, 0); setreg(c, w, 0, getreg(c, w, op & 7)); setreg(c, w, op & 7, a); break; }
    case 0x98: if (d->osz) c->r[R_EAX] = (uint32_t)(int32_t)(int16_t)c->r[R_EAX];
        else setreg(c, 2, R_EAX, (uint16_t)(int16_t)(int8_t)c->r[R_EAX]); break;
    case 0x99: if (d->osz) c->r[R_EDX] = ((int32_t)c->r[R_EAX] < 0) ? 0xFFFFFFFFu : 0;
        else setreg(c, 2, R_EDX, (c->r[R_EAX] & 0x8000) ? 0xFFFF : 0); break;
    case 0x9A: { uint32_t off = d->osz ? fetch32(c) : fetch16(c); uint16_t sg = fetch16(c);
        push(c, c->seg[S_CS], w); push(c, c->eip, w); c->seg[S_CS] = sg; c->eip = off & 0xFFFF; break; }
    case 0x9B: break;
    case 0x9C: push(c, (FL & (d->osz ? 0x00037FD7 : 0x7FD7)) | 2, w); break;
    case 0x9D: load_flags(c, pop(c, w), w); break;
    case 0x9E: FL = (FL & ~0xD5u) | ((c->r[R_EAX] >> 8) & 0xD5) | 2; break;
    case 0x9F: setreg(c, 1, 4, (FL & 0xD7) | 2); break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        uint32_t off = d->asz ? fetch32(c) : fetch16(c);
        uint32_t a = lin(c, d->seg >= 0 ? d->seg : S_DS, off);
        int ww = (op & 1) ? w : 1;
        if (op < 0xA2) setreg(c, ww, 0, rdw(c, a, ww)); else wrw(c, a, getreg(c, ww, 0), ww);
        break; }
    case 0xA8: alu(c, 4, getreg(c, 1, 0), fetch8(c), 1); break;
    case 0xA9: alu(c, 4, getreg(c, w, 0), d->osz ? fetch32(c) : fetch16(c), w); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setreg(c, 1, op & 7, fetch8(c)); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        setreg(c, w, op & 7, d->osz ? fetch32(c) : fetch16(c)); break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int ww = (op & 1) ? w : 1;
        modrm(c, d);
        uint32_t a = rm_read(c, d, ww);
        unsigned cnt = (op <= 0xC1) ? fetch8(c) : (op <= 0xD1) ? 1 : (c->r[R_ECX] & 0xFF);
        uint32_t r = shiftop(c, d->reg, a, cnt, ww);
        if (cnt & 31) rm_write(c, d, ww, r);
        break; }
    case 0xC2: { uint16_t n = fetch16(c); c->eip = pop(c, w) & 0xFFFF;
        c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000u) | ((c->r[R_ESP] + n) & 0xFFFF); break; }
    case 0xC3: c->eip = pop(c, w) & 0xFFFF; break;
    case 0xC4: case 0xC5: { modrm(c, d); if (d->mod == 3) return unimpl(c, d);
        uint32_t off = rdw(c, ealin(c, d, 0), w); uint16_t sg = rd16(c, ealin(c, d, w));
        setreg(c, w, d->reg, off); c->seg[op == 0xC4 ? S_ES : S_DS] = sg; break; }
    case 0xC6: modrm(c, d); if (d->reg) return unimpl(c, d); rm_write(c, d, 1, fetch8(c)); break;
    case 0xC7: modrm(c, d); if (d->reg) return unimpl(c, d); rm_write(c, d, w, d->osz ? fetch32(c) : fetch16(c)); break;
    case 0xC8: {   /* ENTER */
        uint16_t sz = fetch16(c); uint8_t lvl = fetch8(c) & 31;
        push(c, getreg(c, w, R_EBP), w);
        uint32_t frame = c->r[R_ESP] & 0xFFFF;
        if (lvl) {
            for (int i = 1; i < lvl; i++) {
                uint16_t bp = (c->r[R_EBP] - w) & 0xFFFF;
                setreg(c, 2, R_EBP, bp);
                push(c, rdw(c, lin(c, S_SS, bp), w), w);
            }
            push(c, frame, w);
        }
        setreg(c, 2, R_EBP, frame);   /* 16-bit stack: only BP is written (Intel SDM ENTER) */
        c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000u) | ((c->r[R_ESP] - sz) & 0xFFFF);
        break; }
    case 0xC9: c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000u) | (c->r[R_EBP] & 0xFFFF);
        setreg(c, w, R_EBP, pop(c, w)); break;
    case 0xCA: { uint16_t n = fetch16(c); c->eip = pop(c, w) & 0xFFFF; c->seg[S_CS] = pop(c, w);
        c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000u) | ((c->r[R_ESP] + n) & 0xFFFF); break; }
    case 0xCB: c->eip = pop(c, w) & 0xFFFF; c->seg[S_CS] = pop(c, w); break;
    case 0xCC: cpu_interrupt(c, 3); break;
    case 0xCD: { uint8_t v = fetch8(c); cpu_interrupt(c, v); break; }
    case 0xCE: if (GETF(F_OF)) cpu_interrupt(c, 4); break;
    case 0xCF: c->eip = pop(c, w) & 0xFFFF; c->seg[S_CS] = pop(c, w); load_flags(c, pop(c, w), w); break;
    case 0xD4: { uint8_t b = fetch8(c); if (!b) { fault(c, d, 0); break; }
        uint8_t al = c->r[R_EAX] & 0xFF; setreg(c, 1, 4, al / b); setreg(c, 1, 0, al % b); szp(c, al % b, 1);
        FL &= ~(uint32_t)(F_CF | F_OF | F_AF); break; }
    case 0xD5: { uint8_t b = fetch8(c);
        uint8_t al = c->r[R_EAX] & 0xFF, ah = (c->r[R_EAX] >> 8) & 0xFF;
        uint32_t r = alu(c, 0, al, (uint8_t)(ah * b), 1);
        setreg(c, 2, R_EAX, r & 0xFF); break; }
    case 0xD6: setreg(c, 1, 0, GETF(F_CF) ? 0xFF : 0); break;
    case 0xD7: { uint32_t off = (d->asz ? c->r[R_EBX] : (c->r[R_EBX] & 0xFFFF)) + (c->r[R_EAX] & 0xFF);
        if (!d->asz) off &= 0xFFFF;
        setreg(c, 1, 0, rd8(c, lin(c, d->seg >= 0 ? d->seg : S_DS, off))); break; }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm(c, d); break;   /* no FPU present: ESC ops are ignored */
    case 0xE0: case 0xE1: case 0xE2: {
        int8_t rel = fetch8(c);
        cnt_dec(c, d);
        int t = cnt_get(c, d) != 0;
        if (op == 0xE0) t = t && !GETF(F_ZF);
        if (op == 0xE1) t = t && GETF(F_ZF);
        if (t) c->eip = (c->eip + rel) & 0xFFFF;
        break; }
    case 0xE3: { int8_t rel = fetch8(c); if (cnt_get(c, d) == 0) c->eip = (c->eip + rel) & 0xFFFF; break; }
    case 0xE4: setreg(c, 1, 0, c->io_in(c->ud, fetch8(c), 1)); break;
    case 0xE5: { uint8_t p = fetch8(c); setreg(c, w, 0, c->io_in(c->ud, p, w)); break; }
    case 0xE6: { uint8_t p = fetch8(c); c->io_out(c->ud, p, 1, c->r[R_EAX] & 0xFF); break; }
    case 0xE7: { uint8_t p = fetch8(c); c->io_out(c->ud, p, w, getreg(c, w, 0)); break; }
    case 0xE8: { int32_t rel = d->osz ? (int32_t)fetch32(c) : (int16_t)fetch16(c);
        push(c, c->eip, w); c->eip = (c->eip + rel) & 0xFFFF; break; }
    case 0xE9: { int32_t rel = d->osz ? (int32_t)fetch32(c) : (int16_t)fetch16(c); c->eip = (c->eip + rel) & 0xFFFF; break; }
    case 0xEA: { uint32_t off = d->osz ? fetch32(c) : fetch16(c); uint16_t sg = fetch16(c);
        c->seg[S_CS] = sg; c->eip = off & 0xFFFF; break; }
    case 0xEB: { int8_t rel = fetch8(c); c->eip = (c->eip + rel) & 0xFFFF; break; }
    case 0xEC: setreg(c, 1, 0, c->io_in(c->ud, c->r[R_EDX] & 0xFFFF, 1)); break;
    case 0xED: setreg(c, w, 0, c->io_in(c->ud, c->r[R_EDX] & 0xFFFF, w)); break;
    case 0xEE: c->io_out(c->ud, c->r[R_EDX] & 0xFFFF, 1, c->r[R_EAX] & 0xFF); break;
    case 0xEF: c->io_out(c->ud, c->r[R_EDX] & 0xFFFF, w, getreg(c, w, 0)); break;
    case 0xF4: c->halted = 1; break;
    case 0xF5: FL ^= F_CF; break;
    case 0xF6: modrm(c, d); return grp3(c, d, 1);
    case 0xF7: modrm(c, d); return grp3(c, d, w);
    case 0xF8: FL &= ~(uint32_t)F_CF; break;
    case 0xF9: FL |= F_CF; break;
    case 0xFA: FL &= ~(uint32_t)F_IF; break;
    case 0xFB: if (!(FL & F_IF)) c->inhibit_irq = 1; FL |= F_IF; break;
    case 0xFC: FL &= ~(uint32_t)F_DF; break;
    case 0xFD: FL |= F_DF; break;
    case 0xFE: modrm(c, d);
        if (d->reg > 1) return unimpl(c, d);
        rm_write(c, d, 1, incdec(c, rm_read(c, d, 1), 1, d->reg)); break;
    case 0xFF: modrm(c, d);
        switch (d->reg) {
        case 0: case 1: rm_write(c, d, w, incdec(c, rm_read(c, d, w), w, d->reg)); break;
        case 2: { uint32_t t = rm_read(c, d, w); push(c, c->eip, w); c->eip = t & 0xFFFF; break; }
        case 3: { if (d->mod == 3) return unimpl(c, d);
            uint32_t off = rdw(c, ealin(c, d, 0), w); uint16_t sg = rd16(c, ealin(c, d, w));
            push(c, c->seg[S_CS], w); push(c, c->eip, w); c->seg[S_CS] = sg; c->eip = off & 0xFFFF; break; }
        case 4: c->eip = rm_read(c, d, w) & 0xFFFF; break;
        case 5: { if (d->mod == 3) return unimpl(c, d);
            uint32_t off = rdw(c, ealin(c, d, 0), w); uint16_t sg = rd16(c, ealin(c, d, w));
            c->seg[S_CS] = sg; c->eip = off & 0xFFFF; break; }
        case 6: push(c, rm_read(c, d, w), w); break;
        default: return unimpl(c, d);
        }
        break;
    default:
        return unimpl(c, d);
    }
    return 0;
}

int cpu_run(CPU *c, uint64_t n, uint32_t trap_lo, uint32_t trap_hi, uint32_t *trapped) {
    for (uint64_t i = 0; i < n; i++) {
        if (c->halted) return 2;
        uint32_t l = cpu_lin(c);
        if (l >= trap_lo && l < trap_hi) { if (trapped) *trapped = l; return 1; }
        if (cpu_step(c)) return -1;
    }
    return 0;
}

void cpu_init(CPU *c, uint8_t *mem) {
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->a20_mask = 0xFFFFF;
    c->flags = 2;
    for (int i = 0; i < 256; i++) { int p = 0; for (int b = 0; b < 8; b++) p ^= (i >> b) & 1; parity[i] = !p; }
}
