/* bios.c — BIOS HLE: IVT stubs, BIOS data area, INT 10h text video, INT 16h keyboard, INT 1Ah, misc. */
#include "machine.h"
#include <string.h>
#include <time.h>

#define BDA 0x400
static inline uint8_t rb(Machine *m, uint32_t a) { return m->mem[a]; }
static inline uint16_t rw(Machine *m, uint32_t a) { return m->mem[a] | (m->mem[a + 1] << 8); }
static inline void wb(Machine *m, uint32_t a, uint8_t v) { m->mem[a] = v; }
static inline void ww(Machine *m, uint32_t a, uint16_t v) { m->mem[a] = v; m->mem[a + 1] = v >> 8; }
#define AX (m->cpu.r[R_EAX] & 0xFFFF)
#define AH ((m->cpu.r[R_EAX] >> 8) & 0xFF)
#define AL (m->cpu.r[R_EAX] & 0xFF)
#define BX (m->cpu.r[R_EBX] & 0xFFFF)
#define CX (m->cpu.r[R_ECX] & 0xFFFF)
#define DX (m->cpu.r[R_EDX] & 0xFFFF)
static inline void set16(Machine *m, int r, uint16_t v) { m->cpu.r[r] = (m->cpu.r[r] & 0xFFFF0000u) | v; }
static inline void setAL(Machine *m, uint8_t v) { m->cpu.r[R_EAX] = (m->cpu.r[R_EAX] & ~0xFFu) | v; }
static inline void setAH(Machine *m, uint8_t v) { m->cpu.r[R_EAX] = (m->cpu.r[R_EAX] & ~0xFF00u) | (v << 8); }

void bios_init(Machine *m) {
    for (int n = 0; n < 256; n++) {
        ww(m, n * 4, n); ww(m, n * 4 + 2, STUBSEG);
        wb(m, STUBSEG * 16 + n, 0xCF);           /* IRET (never executed: trapped) */
    }
    ww(m, BDA + 0x10, 0x0023);                    /* equipment: 80x25 color, 1 floppy bit off */
    ww(m, BDA + 0x13, 640);
    ww(m, BDA + 0x1A, 0x1E); ww(m, BDA + 0x1C, 0x1E); ww(m, BDA + 0x80, 0x1E); ww(m, BDA + 0x82, 0x3E);
    wb(m, BDA + 0x49, 3); ww(m, BDA + 0x4A, 80); ww(m, BDA + 0x4C, 4096); ww(m, BDA + 0x63, 0x3D4);
    wb(m, BDA + 0x84, 24); ww(m, BDA + 0x85, 16);
    wb(m, BDA + 0x87, 0x60); wb(m, BDA + 0x88, 0x09); wb(m, BDA + 0x89, 0x11);
    memcpy(m->mem + 0xFFFF5, "12/25/94", 8);
    wb(m, 0xFFFFE, 0xFC);
    for (int i = 0; i < 80 * 50; i++) ww(m, 0xB8000 + i * 2, 0x0720);
}

/* ------------------------------------------------------------------ text helpers */
static int rows(Machine *m) { return rb(m, BDA + 0x84) + 1; }
static void scroll(Machine *m, int up, int n, int top, int left, int bot, int right, uint8_t attr) {
    int cols = 80, nrows = bot - top + 1;
    if (n == 0 || n > nrows) n = nrows;
    for (int r = 0; r < nrows; r++) {
        int dst = up ? top + r : bot - r, src = up ? dst + n : dst - n;
        for (int c = left; c <= right; c++) {
            uint32_t d = 0xB8000 + (dst * cols + c) * 2;
            if ((up && src <= bot) || (!up && src >= top)) ww(m, d, rw(m, 0xB8000 + (src * cols + c) * 2));
            else ww(m, d, (attr << 8) | 0x20);
        }
    }
}
void bios_tty(Machine *m, uint8_t ch) {
    uint16_t pos = rw(m, BDA + 0x50); int r = pos >> 8, c = pos & 0xFF, nr = rows(m);
    if (ch == 0x0D) c = 0;
    else if (ch == 0x0A) r++;
    else if (ch == 0x08) { if (c) c--; }
    else if (ch == 0x07) {}
    else { wb(m, 0xB8000 + (r * 80 + c) * 2, ch); if (++c >= 80) { c = 0; r++; } }
    if (r >= nr) { scroll(m, 1, 1, 0, 0, nr - 1, 79, 0x07); r = nr - 1; }
    ww(m, BDA + 0x50, (r << 8) | c);
    m->video_dirty = 1;
}

/* ------------------------------------------------------------------ INT 10h */
static int int10(Machine *m, uint16_t *fl) {
    (void)fl;
    switch (AH) {
    case 0x00:
        wb(m, BDA + 0x49, AL & 0x7F); wb(m, BDA + 0x84, 24); ww(m, BDA + 0x85, 16); ww(m, BDA + 0x50, 0);
        if (!(AL & 0x80)) for (int i = 0; i < 80 * 50; i++) ww(m, 0xB8000 + i * 2, 0x0720);
        m->video_dirty = 1; return 1;
    case 0x01: ww(m, BDA + 0x60, CX); m->crtc[0x0A] = CX >> 8; m->crtc[0x0B] = CX & 0xFF; return 1;
    case 0x02: ww(m, BDA + 0x50, DX); m->video_dirty = 1; return 1;
    case 0x03: set16(m, R_EDX, rw(m, BDA + 0x50)); set16(m, R_ECX, rw(m, BDA + 0x60)); return 1;
    case 0x05: return 1;
    case 0x06: case 0x07:
        scroll(m, AH == 6, AL, CX >> 8, CX & 0xFF, DX >> 8, DX & 0xFF, BX >> 8); m->video_dirty = 1; return 1;
    case 0x08: { uint16_t p = rw(m, BDA + 0x50); set16(m, R_EAX, rw(m, 0xB8000 + ((p >> 8) * 80 + (p & 0xFF)) * 2)); return 1; }
    case 0x09: case 0x0A: {
        uint16_t p = rw(m, BDA + 0x50); uint32_t a = 0xB8000 + ((p >> 8) * 80 + (p & 0xFF)) * 2;
        for (int k = 0; k < CX && a + k * 2 < 0xC0000; k++) { wb(m, a + k * 2, AL); if (AH == 9) wb(m, a + k * 2 + 1, BX & 0xFF); }
        m->video_dirty = 1; return 1; }
    case 0x0E: bios_tty(m, AL); return 1;
    case 0x0F: set16(m, R_EAX, (80 << 8) | rb(m, BDA + 0x49)); set16(m, R_EBX, BX & 0xFF); return 1;
    case 0x10:
        switch (AL) {
        case 0x00: m->attr[BX & 0x0F] = BX >> 8; break;
        case 0x01: m->attr[0x11] = BX >> 8; break;
        case 0x02: for (int i = 0; i < 17; i++) m->attr[i == 16 ? 0x11 : i] = m->mem[((m->cpu.seg[S_ES] << 4) + DX + i) & 0xFFFFF]; break;
        case 0x03: if (BX & 1) m->attr[0x10] |= 0x08; else m->attr[0x10] &= ~0x08; break;
        case 0x10: m->dac[(BX & 0xFF) * 3] = (DX >> 8) & 0x3F; m->dac[(BX & 0xFF) * 3 + 1] = (CX >> 8) & 0x3F; m->dac[(BX & 0xFF) * 3 + 2] = CX & 0x3F; break;
        case 0x12: for (int i = 0; i < CX * 3 && (BX & 0xFF) * 3 + i < 768; i++)
                       m->dac[(BX & 0xFF) * 3 + i] = m->mem[((m->cpu.seg[S_ES] << 4) + DX + i) & 0xFFFFF] & 0x3F; break;
        case 0x15: set16(m, R_EDX, m->dac[(BX & 0xFF) * 3] << 8); set16(m, R_ECX, (m->dac[(BX & 0xFF) * 3 + 1] << 8) | m->dac[(BX & 0xFF) * 3 + 2]); break;
        }
        m->video_dirty = 1; return 1;
    case 0x11:
        if (AL == 0x12 || AL == 0x02) { wb(m, BDA + 0x84, 49); ww(m, BDA + 0x85, 8); }
        else if (AL == 0x14 || AL == 0x04 || AL == 0x11 || AL == 0x01) { wb(m, BDA + 0x84, 24); ww(m, BDA + 0x85, 16); }
        else if (AL == 0x30) { set16(m, R_ECX, rw(m, BDA + 0x85)); set16(m, R_EDX, rb(m, BDA + 0x84)); }
        m->video_dirty = 1; return 1;
    case 0x12: if ((BX & 0xFF) == 0x10) { set16(m, R_EBX, 0x0003); set16(m, R_ECX, 0x0009); } return 1;
    case 0x1A: if (AL == 0) { set16(m, R_EAX, 0x001A); set16(m, R_EBX, 0x0008); } return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ INT 16h */
/* returns 1 handled, 2 = must block (no key yet) */
static int int16(Machine *m, uint16_t *fl) {
    switch (AH) {
    case 0x00: case 0x10:
        if (m->kq_head == m->kq_tail) return 2;
        set16(m, R_EAX, m->keyq[m->kq_head]); m->kq_head = (m->kq_head + 1) % KEYQ; return 1;
    case 0x01: case 0x11:
        if (m->kq_head == m->kq_tail) { *fl |= F_ZF; return 1; }
        set16(m, R_EAX, m->keyq[m->kq_head]); *fl &= ~F_ZF; return 1;
    case 0x02: case 0x12: setAL(m, m->shift_flags & 0xFF); if (AH == 0x12) setAH(m, m->shift_flags >> 8); return 1;
    case 0x03: case 0x05: return 1;
    }
    return 0;
}

void machine_key(Machine *m, uint16_t key) {
    int nt = (m->kq_tail + 1) % KEYQ;
    if (nt == m->kq_head) return;
    m->keyq[m->kq_tail] = key; m->kq_tail = nt;
}
int machine_keyq_free(Machine *m) { return KEYQ - 1 - ((m->kq_tail - m->kq_head + KEYQ) % KEYQ); }

/* ------------------------------------------------------------------ dispatcher */
int bios_service(Machine *m, int vec, uint16_t *fl) {
    switch (vec) {
    case 0x10: return int10(m, fl);
    case 0x16: return int16(m, fl);
    case 0x03: return 1;   /* INT 3: a stray breakpoint in ST3's FM note-on code; DOS default handler is IRET */
    case 0x21: return dos_service(m, fl);
    case 0x20: m->exited = 1; m->exit_code = 0; return 1;
    case 0x08: {
        uint32_t t = rw(m, BDA + 0x6C) | ((uint32_t)rw(m, BDA + 0x6E) << 16); t++;
        if (t >= 0x1800B0) { t = 0; wb(m, BDA + 0x70, 1); }
        ww(m, BDA + 0x6C, t); ww(m, BDA + 0x6E, t >> 16);
        m->isr &= ~1; return 1; }
    case 0x09: m->isr &= ~2; return 1;
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: m->isr &= ~(1 << (vec - 8)); return 1;
    case 0x1A:
        if (AH == 0) { uint32_t t = rw(m, BDA + 0x6C) | ((uint32_t)rw(m, BDA + 0x6E) << 16);
            set16(m, R_ECX, t >> 16); set16(m, R_EDX, t & 0xFFFF); setAL(m, rb(m, BDA + 0x70)); wb(m, BDA + 0x70, 0); }
        else if (AH == 2 || AH == 4) {
            time_t now = time(NULL); struct tm *lt = localtime(&now);
#define BCD(x) ((uint8_t)((((x) / 10) << 4) | ((x) % 10)))
            if (AH == 2) { set16(m, R_ECX, (BCD(lt->tm_hour) << 8) | BCD(lt->tm_min)); set16(m, R_EDX, BCD(lt->tm_sec) << 8); }
            else { int y = lt->tm_year + 1900; set16(m, R_ECX, (BCD(y / 100) << 8) | BCD(y % 100)); set16(m, R_EDX, (BCD(lt->tm_mon + 1) << 8) | BCD(lt->tm_mday)); }
            *fl &= ~F_CF;
        }
        return 1;
    case 0x15: *fl |= F_CF; setAH(m, 0x86); return 1;
    case 0x2F:
        if (AX == 0x4300 || AX == 0x1600) setAL(m, 0);
        return 1;
    case 0x33: if (AX == 0) set16(m, R_EAX, 0); return 1;
    case 0x1C: case 0x23: case 0x05: case 0x1B: return 1;
    case 0x24: setAL(m, 3); return 1;
    case 0x28: return 1;
    case 0x00:
        LOGF(m, "Divide overflow (INT 0 default handler) -> exit\n");
        m->exited = 1; m->exit_code = 0xFF; return 1;
    case 0x67: set16(m, R_EAX, 0x8400); return 1;   /* EMS: "software malfunction" if anyone calls */
    }
    return 0;
}
