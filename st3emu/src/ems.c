/* ems.c — LIM EMS 4.0 expanded memory (what EMM386/QEMM gave ST3 under DOS): 16 MB in 16 KB pages, a 64 KB page
 * frame at D000h with 4 physical pages, INT 67h services, and the "EMMXXXX0" device that programs look for.
 *
 * Mapping copies: the 4 physical pages are real memory at D0000h; mapping a logical page writes the page it replaces
 * back to the EMS store and copies the new one in.  The CPU core needs no changes, and the frame always holds exactly
 * what a real board would show.  (A logical page mapped into two physical pages at once is not kept coherent; no
 * program we run does that.)
 *
 * ST3 3.21 uses it this way (disassembly): fopen("EMMXXXX0") -> 41h page frame -> 43h one page for its context
 * (47h/48h save/restore around the player) -> 42h free pages (FreeEMS display) -> one 43h handle per sample of up to
 * 4 pages; the player maps pages 0..3 of a sample's handle with 44h and reads the sample from the frame. */
#include "machine.h"
#include <stdlib.h>
#include <string.h>

#define AH ((m->cpu.r[R_EAX] >> 8) & 0xFF)
#define AL (m->cpu.r[R_EAX] & 0xFF)
#define BX (m->cpu.r[R_EBX] & 0xFFFF)
#define CX (m->cpu.r[R_ECX] & 0xFFFF)
#define DX (m->cpu.r[R_EDX] & 0xFFFF)
static inline void set16(Machine *m, int r, uint16_t v) { m->cpu.r[r] = (m->cpu.r[r] & 0xFFFF0000u) | v; }
static inline void setAH(Machine *m, uint8_t v) { m->cpu.r[R_EAX] = (m->cpu.r[R_EAX] & ~0xFF00u) | (v << 8); }
static inline void setAL(Machine *m, uint8_t v) { m->cpu.r[R_EAX] = (m->cpu.r[R_EAX] & ~0xFFu) | v; }
static inline uint16_t rw(Machine *m, uint32_t a) { return m->mem[a] | (m->mem[a + 1] << 8); }
static inline void ww(Machine *m, uint32_t a, uint16_t v) { m->mem[a] = v; m->mem[a + 1] = v >> 8; }
static inline uint32_t lin(uint16_t seg, uint16_t off) { return ((uint32_t)seg * 16 + off) & 0xFFFFF; }

#define PG 16384u
#define FRAME_LIN ((uint32_t)EMS_FRAME_SEG * 16)

static Ems *E(Machine *m) { return &m->ems; }

void ems_init(Machine *m) {
    Ems *e = E(m);
    memset(e, 0, sizeof *e);
    if (m->cfg.no_ems) return;                /* no driver: INT 67h answers 80h, EMMXXXX0 does not exist */
    e->store = calloc(EMS_PAGES, PG);          /* untouched pages cost no host memory */
    for (int p = 0; p < 4; p++) e->phys[p] = -1;
    e->handle[0].used = 1;                    /* handle 0 = operating system handle, 0 pages */
    e->free_pages = EMS_PAGES;
    /* INT 67h vector: EFFE:0087 is the trap stub at F000:0067; "EMMXXXX0" at EFFE:000A, where programs that check
     * the driver's device header (vector segment : 000Ah) look for it */
    ww(m, 0x67 * 4, 0x0087); ww(m, 0x67 * 4 + 2, 0xEFFE);
    memcpy(m->mem + 0xEFFE0 + 0x0A, "EMMXXXX0", 8);
}

/* ---- mapping ---- */
static void map_phys(Machine *m, int p, int g) {
    Ems *e = E(m);
    if (e->phys[p] == g) return;
    if (e->phys[p] >= 0) memcpy(e->store + (size_t)e->phys[p] * PG, m->mem + FRAME_LIN + p * PG, PG);
    if (g >= 0) memcpy(m->mem + FRAME_LIN + p * PG, e->store + (size_t)g * PG, PG);
    e->phys[p] = g;
}
static int handle_ok(Machine *m, int h) { return h >= 0 && h < EMS_HANDLES && E(m)->handle[h].used; }
static int alloc_pages(Machine *m, int h, int n) {           /* append n pages to handle h; 0 = ok */
    Ems *e = E(m);
    if (n > e->free_pages) return -1;
    uint16_t *np = realloc(e->handle[h].pages, sizeof(uint16_t) * (e->handle[h].n + n + 1));
    if (!np) return -1;
    e->handle[h].pages = np;
    for (int g = 0; g < EMS_PAGES && n > 0; g++)
        if (!e->owner[g]) { e->owner[g] = (uint8_t)(h + 1); np[e->handle[h].n++] = (uint16_t)g; e->free_pages--; n--;
                            memset(e->store + (size_t)g * PG, 0, PG); }
    return 0;
}
static void free_pages_from(Machine *m, int h, int keep) {   /* shrink handle h to keep pages */
    Ems *e = E(m);
    while (e->handle[h].n > keep) {
        int g = e->handle[h].pages[--e->handle[h].n];
        for (int p = 0; p < 4; p++) if (e->phys[p] == g) e->phys[p] = -1;   /* page gone: frame contents undefined */
        e->owner[g] = 0; e->free_pages++;
    }
}
static int new_handle(Machine *m) {
    for (int h = 1; h < EMS_HANDLES; h++) if (!E(m)->handle[h].used) { memset(&E(m)->handle[h], 0, sizeof E(m)->handle[h]); E(m)->handle[h].used = 1; return h; }
    return -1;
}
/* logical page l of handle h -> global page, or -1 (l == 0xFFFF: unmap) ; -2 = out of range */
static int logical(Machine *m, int h, unsigned l) {
    if (l == 0xFFFF) return -1;
    return l < (unsigned)E(m)->handle[h].n ? E(m)->handle[h].pages[l] : -2;
}
static int seg_to_phys(uint16_t seg) { return (seg >= EMS_FRAME_SEG && seg < EMS_FRAME_SEG + 0x1000 && !(seg & 0x3FF)) ? (seg - EMS_FRAME_SEG) >> 10 : -1; }

/* ---- context save format (4Eh/4Fh/47h): 4 words = global page + 1 (0 = unmapped) ---- */
static void ctx_get(Machine *m, uint32_t a) { for (int p = 0; p < 4; p++) ww(m, a + p * 2, (uint16_t)(E(m)->phys[p] + 1)); }
static void ctx_set(Machine *m, uint32_t a) {
    for (int p = 0; p < 4; p++) { int g = (int)rw(m, a + p * 2) - 1; if (g >= EMS_PAGES || (g >= 0 && !E(m)->owner[g])) g = -1; map_phys(m, p, g); }
}

int ems_int67(Machine *m) {
    Ems *e = E(m);
    int st = 0;
    uint16_t es = m->cpu.seg[S_ES], ds = m->cpu.seg[S_DS];
    uint16_t di = m->cpu.r[R_EDI] & 0xFFFF, si = m->cpu.r[R_ESI] & 0xFFFF;
    if (!e->store) { setAH(m, 0x80); return 1; }
    int fn = AH;
    switch (AH) {
    case 0x40: break;                                                    /* status */
    case 0x41: set16(m, R_EBX, EMS_FRAME_SEG); break;                    /* page frame segment */
    case 0x42: set16(m, R_EBX, e->free_pages); set16(m, R_EDX, EMS_PAGES); break;
    case 0x43: case 0x5A: {                                              /* allocate (5Ah: LIM 4.0, 0 pages ok) */
        int n = BX;
        if (AH == 0x43 && n == 0) { st = 0x89; break; }
        if (n > EMS_PAGES) { st = 0x87; break; }
        if (n > e->free_pages) { st = 0x88; break; }
        int h = new_handle(m); if (h < 0) { st = 0x85; break; }
        if (alloc_pages(m, h, n)) { e->handle[h].used = 0; st = 0x88; break; }
        set16(m, R_EDX, (uint16_t)h); break; }
    case 0x44: {                                                         /* map one page */
        if (!handle_ok(m, DX)) { st = 0x83; break; }
        if (AL > 3) { st = 0x8B; break; }
        int g = logical(m, DX, BX); if (g == -2) { st = 0x8A; break; }
        map_phys(m, AL, g); break; }
    case 0x45:                                                           /* deallocate */
        if (DX == 0) { free_pages_from(m, 0, 0); break; }                 /* OS handle: frees its pages, stays */
        if (!handle_ok(m, DX)) { st = 0x83; break; }
        if (e->handle[DX].saved) { st = 0x86; break; }
        free_pages_from(m, DX, 0); free(e->handle[DX].pages); memset(&e->handle[DX], 0, sizeof e->handle[DX]); break;
    case 0x46: setAL(m, 0x40); break;                                    /* version 4.0 */
    case 0x47:                                                           /* save page map (in handle) */
        if (!handle_ok(m, DX)) { st = 0x83; break; }
        if (e->handle[DX].saved) { st = 0x8D; break; }
        for (int p = 0; p < 4; p++) e->handle[DX].ctx[p] = e->phys[p];
        e->handle[DX].saved = 1; break;
    case 0x48:                                                           /* restore page map */
        if (!handle_ok(m, DX)) { st = 0x83; break; }
        if (!e->handle[DX].saved) { st = 0x8E; break; }
        for (int p = 0; p < 4; p++) map_phys(m, p, e->handle[DX].ctx[p]);
        e->handle[DX].saved = 0; break;
    case 0x4B: { int n = 0; for (int h = 0; h < EMS_HANDLES; h++) n += e->handle[h].used; set16(m, R_EBX, n); break; }
    case 0x4C: if (!handle_ok(m, DX)) { st = 0x83; break; } set16(m, R_EBX, e->handle[DX].n); break;
    case 0x4D: { int n = 0; uint32_t a = lin(es, di);
        for (int h = 0; h < EMS_HANDLES; h++) if (e->handle[h].used) { ww(m, a + n * 4, h); ww(m, a + n * 4 + 2, e->handle[h].n); n++; }
        set16(m, R_EBX, n); break; }
    case 0x4E:                                                           /* get/set whole page map */
        if (AL == 0 || AL == 2) ctx_get(m, lin(es, di));
        if (AL == 1 || AL == 2) ctx_set(m, lin(ds, si));
        if (AL == 3) setAL(m, 8);
        if (AL > 3) st = 0x8F;
        break;
    case 0x4F:                                                           /* partial page map: we save all 4 */
        if (AL == 0) { ww(m, lin(es, di), 4); ctx_get(m, lin(es, di) + 2); }
        else if (AL == 1) ctx_set(m, lin(ds, si) + 2);
        else if (AL == 2) setAL(m, 10);
        else st = 0x8F;
        break;
    case 0x50: {                                                         /* map multiple */
        if (!handle_ok(m, DX)) { st = 0x83; break; }
        if (AL > 1) { st = 0x8F; break; }
        uint32_t a = lin(ds, si);
        for (int i = 0; i < CX && !st; i++) {
            int l = rw(m, a + i * 4), pp = rw(m, a + i * 4 + 2);
            int p = AL ? seg_to_phys((uint16_t)pp) : pp;
            if (p < 0 || p > 3) { st = 0x8B; break; }
            int g = logical(m, DX, l); if (g == -2) { st = 0x8A; break; }
            map_phys(m, p, g);
        }
        break; }
    case 0x51: {                                                         /* reallocate */
        if (!handle_ok(m, DX)) { st = 0x83; break; }
        int n = BX;
        if (n > e->handle[DX].n) { if (n - e->handle[DX].n > e->free_pages) { st = 0x88; break; } alloc_pages(m, DX, n - e->handle[DX].n); }
        else free_pages_from(m, DX, n);
        set16(m, R_EBX, e->handle[DX].n); break; }
    case 0x53:                                                           /* handle name */
        if (!handle_ok(m, DX)) { st = 0x83; break; }
        if (AL == 0) memcpy(m->mem + lin(es, di), e->handle[DX].name, 8);
        else if (AL == 1) memcpy(e->handle[DX].name, m->mem + lin(ds, si), 8);
        else st = 0x8F;
        break;
    case 0x58:                                                           /* mappable physical address array */
        if (AL == 0) for (int p = 0; p < 4; p++) { ww(m, lin(es, di) + p * 4, EMS_FRAME_SEG + p * 0x400); ww(m, lin(es, di) + p * 4 + 2, p); }
        else if (AL != 1) { st = 0x8F; break; }
        set16(m, R_ECX, 4); break;
    case 0x59:                                                           /* hardware info: raw page counts */
        if (AL == 1) { set16(m, R_EBX, e->free_pages); set16(m, R_EDX, EMS_PAGES); }
        else st = 0xA4;                                                  /* access denied (as EMM386 does) */
        break;
    default:
        st = 0x84;
        if (e->unknown_reported++ < 20) LOGF(m, "EMS: unsupported INT 67h AX=%04X\n", m->cpu.r[R_EAX] & 0xFFFF);
    }
    setAH(m, (uint8_t)st);
    if (fn != 0x44 && fn != 0x47 && fn != 0x48 && fn != 0x42)   /* the player's mapping calls and the FreeEMS poll are not logged */
        LOGF(m, "EMS %02Xh BX=%04X DX=%04X -> AH=%02X BX=%04X DX=%04X (free %d)\n", fn, BX, DX, st,
             m->cpu.r[R_EBX] & 0xFFFF, m->cpu.r[R_EDX] & 0xFFFF, e->free_pages);
    return 1;
}
