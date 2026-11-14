/* machine.c — machine init, EXE loader, run loop (HLE traps, IRQ delivery, idle), state dump. */
#include "machine.h"
#include <string.h>
#include <stdlib.h>

#define STUB_LIN (STUBSEG * 16)

static inline uint16_t rw(Machine *m, uint32_t a) { return m->mem[a] | (m->mem[a + 1] << 8); }
static inline void ww(Machine *m, uint32_t a, uint16_t v) { m->mem[a] = v; m->mem[a + 1] = v >> 8; }

void machine_init(Machine *m, uint8_t *mem, const MachineConfig *cfg, FILE *log) {
    memset(m, 0, sizeof *m);
    memset(mem, 0, MEM_SIZE);
    m->mem = mem; m->cfg = *cfg; m->log = log;
    cpu_init(&m->cpu, mem);
    m->cpu.io_in = io_in; m->cpu.io_out = io_out; m->cpu.ud = m;
    m->pic_mask = 0xB8;
    m->pit_div = 0; m->pit_next = 65536 / 1193182.0;
    m->sb.cmd = -1;
    vga_init(m);
    opl_reset(&m->opl);
    bios_init(m);
    dos_init(m);
}

int machine_load_exe(Machine *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(sz);
    if (fread(d, 1, sz, f) != (size_t)sz) { fclose(f); free(d); return -1; }
    fclose(f);
    int r = machine_load_exe_mem(m, d, sz, path);
    free(d);
    return r;
}

int machine_load_exe_mem(Machine *m, const uint8_t *d, long sz, const char *name) {
    const char *path = name;
    if (sz < 32 || d[0] != 'M' || d[1] != 'Z') return -2;
    uint16_t lastpg = d[2] | d[3] << 8, pages = d[4] | d[5] << 8, nrel = d[6] | d[7] << 8, hdr = d[8] | d[9] << 8;
    uint16_t ss = d[14] | d[15] << 8, sp = d[16] | d[17] << 8, ip = d[20] | d[21] << 8, cs = d[22] | d[23] << 8, reloff = d[24] | d[25] << 8;
    long imgsz = (long)(pages - 1) * 512 + (lastpg ? lastpg : 512) - hdr * 16;
    uint16_t load = PSP_SEG + 0x10;
    if ((long)load * 16 + imgsz > MEM_TOP * 16 || hdr * 16 + imgsz > sz) return -3;
    memcpy(m->mem + load * 16, d + hdr * 16, imgsz);
    for (int i = 0; i < nrel; i++) {
        uint16_t o = d[reloff + i * 4] | d[reloff + i * 4 + 1] << 8, s = d[reloff + i * 4 + 2] | d[reloff + i * 4 + 3] << 8;
        uint32_t a = (uint32_t)(load + s) * 16 + o;
        ww(m, a, rw(m, a) + load);
    }
    /* environment */
    uint32_t e = ENV_SEG * 16; int k = 0;
    const char *base[] = {"COMSPEC=C:\\COMMAND.COM", "PATH=C:\\"};
    for (int i = 0; i < 2; i++) { size_t l = strlen(base[i]) + 1; memcpy(m->mem + e + k, base[i], l); k += l; }
    memcpy(m->mem + e + k, m->cfg.env, m->cfg.envlen); k += m->cfg.envlen;
    m->mem[e + k++] = 0; m->mem[e + k++] = 1; m->mem[e + k++] = 0;
    const char *prog = "C:\\ST3\\ST3.EXE"; memcpy(m->mem + e + k, prog, strlen(prog) + 1);
    /* PSP */
    uint32_t p = PSP_SEG * 16;
    m->mem[p] = 0xCD; m->mem[p + 1] = 0x20; ww(m, p + 2, MEM_TOP); ww(m, p + 0x2C, ENV_SEG);
    uint8_t jft[20] = {1, 1, 1, 0, 2}; memset(jft + 5, 0xFF, 15); memcpy(m->mem + p + 0x18, jft, 20);
    ww(m, p + 0x32, 20); ww(m, p + 0x34, 0x18); ww(m, p + 0x36, PSP_SEG);
    const char *args = m->cfg.args ? m->cfg.args : "";
    int al = strlen(args); if (al > 125) al = 125;
    m->mem[p + 0x80] = al ? al + 1 : 0;
    if (al) { m->mem[p + 0x81] = ' '; memcpy(m->mem + p + 0x82, args, al); m->mem[p + 0x82 + al] = 0x0D; }
    else m->mem[p + 0x81] = 0x0D;
    /* registers */
    CPU *c = &m->cpu;
    c->seg[S_CS] = load + cs; c->eip = ip; c->seg[S_SS] = load + ss; c->r[R_ESP] = sp;
    c->seg[S_DS] = c->seg[S_ES] = PSP_SEG; c->flags = 0x0202;
    LOGF(m, "loaded %s: %ld bytes, %u relocs, entry %04X:%04X\n", path, imgsz, nrel, cs, ip);
    return 0;
}

static void deliver_irqs(Machine *m, int force) {
    CPU *c = &m->cpu;
    if (!force && (!(c->flags & F_IF) || c->inhibit_irq)) return;
    for (int irq = 0; irq < 8; irq++) {
        if (m->isr & ((2 << irq) - 1)) return;          /* same/higher priority in service */
        if (m->irr & ~m->pic_mask & (1 << irq)) {
            m->irr &= ~(1 << irq); m->isr |= 1 << irq;
            cpu_interrupt(c, 0x08 + irq);
            return;
        }
    }
}

int machine_run(Machine *m, uint64_t n) {
    CPU *c = &m->cpu;
    uint64_t end = c->icount + n;
    const uint64_t CHUNK = 1000;
    while (c->icount < end && !m->exited) {
        uint32_t trapped = 0;
        int r = cpu_run(c, CHUNK, STUB_LIN, STUB_LIN + 0x100, &trapped);
        int idle = 0, force = 0;
        if (r < 0) {
            LOGF(m, "CPU error at %05X: %02X %02X %02X %02X %02X %02X\n", c->err_lin, c->err_bytes[0], c->err_bytes[1],
                 c->err_bytes[2], c->err_bytes[3], c->err_bytes[4], c->err_bytes[5]);
            return -1;
        }
        if (r == 1) {
            int vec = trapped - STUB_LIN;
            uint32_t sb = (uint32_t)c->seg[S_SS] * 16;
            uint16_t sp = c->r[R_ESP] & 0xFFFF;
            uint16_t ip = rw(m, sb + sp), cs = rw(m, sb + (uint16_t)(sp + 2)), fl = rw(m, sb + (uint16_t)(sp + 4));
            m->ints_seen[vec]++;
            int h = bios_service(m, vec, &fl);
            if (h == 2) { idle = 1; force = 1; }        /* blocking service: wait (with IRQs) and retry */
            else {
                if (h == 0 && m->unhandled_reported < 200) {
                    m->unhandled_reported++;
                    LOGF(m, "UNHANDLED INT %02X AX=%04X BX=%04X CX=%04X DX=%04X from %04X:%04X\n", vec, c->r[R_EAX] & 0xFFFF,
                         c->r[R_EBX] & 0xFFFF, c->r[R_ECX] & 0xFFFF, c->r[R_EDX] & 0xFFFF, cs, ip);
                }
                c->r[R_ESP] = (c->r[R_ESP] & 0xFFFF0000u) | (uint16_t)(sp + 6);
                c->seg[S_CS] = cs; c->eip = ip; c->flags = (c->flags & 0xFFFF0000u) | (fl & 0x7FD5) | 2;
                c->icount += 20;   /* HLE costs a little virtual time */
            }
        } else if (r == 2) idle = 1;   /* HLT */
        if (idle) c->icount += CHUNK;
        timers_update(m);
        mixer_update(m);
        if (m->irr) deliver_irqs(m, force);
    }
    return 0;
}

void machine_dump_vga_state(Machine *m, const char *json_path, const char *font_path) {
    FILE *f = fopen(font_path, "wb");
    if (f) { fwrite(m->font, 1, sizeof m->font, f); fclose(f); }
    f = fopen(json_path, "w");
    if (!f) return;
    fprintf(f, "{\"dac\": [");
    for (int i = 0; i < 768; i++) fprintf(f, "%s%d", i ? "," : "", m->dac[i]);
    fprintf(f, "], \"attr\": [");
    for (int i = 0; i < 21; i++) fprintf(f, "%s%d", i ? "," : "", m->attr[i]);
    fprintf(f, "], \"rows\": %d, \"text\": [", m->mem[0x484] + 1);
    for (int i = 0; i < 80 * 50 * 2; i++) fprintf(f, "%s%d", i ? "," : "", m->mem[0xB8000 + i]);
    fprintf(f, "], \"cursor\": %d, \"charh\": %d, \"seq3\": %d, \"misc\": %d", rw(m, 0x450), rw(m, 0x485), m->seq[3], m->misc_out);
    fprintf(f, ", \"seq\": ["); for (int i = 0; i < 8; i++) fprintf(f, "%s%d", i ? "," : "", m->seq[i]);
    fprintf(f, "], \"crtc\": ["); for (int i = 0; i < 25; i++) fprintf(f, "%s%d", i ? "," : "", m->crtc[i]);
    fprintf(f, "], \"gc\": ["); for (int i = 0; i < 9; i++) fprintf(f, "%s%d", i ? "," : "", m->gc[i]);
    fprintf(f, "]}\n");
    fclose(f);
}
