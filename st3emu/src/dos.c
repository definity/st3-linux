/* dos.c — DOS INT 21h HLE: MCB-chain memory, PSP/env, host-sandboxed file system with 8.3 mapping, FINDFIRST. */
#include "machine.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static inline uint8_t rb(Machine *m, uint32_t a) { return m->mem[a & 0xFFFFF]; }
static inline uint16_t rw(Machine *m, uint32_t a) { return rb(m, a) | (rb(m, a + 1) << 8); }
static inline void wb(Machine *m, uint32_t a, uint8_t v) { m->mem[a & 0xFFFFF] = v; }
static inline void ww(Machine *m, uint32_t a, uint16_t v) { wb(m, a, v); wb(m, a + 1, v >> 8); }
#define R16(x) (m->cpu.r[x] & 0xFFFF)
#define AX R16(R_EAX)
#define AH ((m->cpu.r[R_EAX] >> 8) & 0xFF)
#define AL (m->cpu.r[R_EAX] & 0xFF)
#define BX R16(R_EBX)
#define CX R16(R_ECX)
#define DX R16(R_EDX)
#define SI R16(R_ESI)
#define DI R16(R_EDI)
#define DS m->cpu.seg[S_DS]
#define ES m->cpu.seg[S_ES]
static inline void set16(Machine *m, int r, uint16_t v) { m->cpu.r[r] = (m->cpu.r[r] & 0xFFFF0000u) | v; }
static inline void setAL(Machine *m, uint8_t v) { m->cpu.r[R_EAX] = (m->cpu.r[R_EAX] & ~0xFFu) | v; }
void bios_tty(Machine *m, uint8_t ch);

static void asciiz(Machine *m, uint16_t seg, uint16_t off, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { uint8_t c = rb(m, ((uint32_t)seg << 4) + (uint16_t)(off + i)); if (!c) break; out[i] = c; }
    out[i] = 0;
}

/* ------------------------------------------------------------------ MCB chain */
#define FIRST_MCB (ENV_SEG - 1)
static void mcb_w(Machine *m, uint16_t s, uint8_t sig, uint16_t owner, uint16_t size) {
    wb(m, s * 16, sig); ww(m, s * 16 + 1, owner); ww(m, s * 16 + 3, size);
}
static int mcb_ok(Machine *m, uint16_t s) { return s >= FIRST_MCB && s < MEM_TOP && (rb(m, s * 16) == 'M' || rb(m, s * 16) == 'Z'); }
static uint8_t mcb_sig(Machine *m, uint16_t s) { return rb(m, s * 16); }
static uint16_t mcb_own(Machine *m, uint16_t s) { return rw(m, s * 16 + 1); }
static uint16_t mcb_size(Machine *m, uint16_t s) { return rw(m, s * 16 + 3); }

static void mcb_merge(Machine *m) {
    uint16_t s = FIRST_MCB;
    for (int guard = 0; guard < 4096 && mcb_ok(m, s); guard++) {
        if (mcb_sig(m, s) != 'M') return;
        uint16_t n = s + mcb_size(m, s) + 1;
        if (!mcb_ok(m, n)) return;
        if (mcb_own(m, s) == 0 && mcb_own(m, n) == 0) { mcb_w(m, s, mcb_sig(m, n), 0, mcb_size(m, s) + mcb_size(m, n) + 1); continue; }
        s = n;
    }
}
static uint16_t mcb_maxfree(Machine *m) {
    uint16_t s = FIRST_MCB, best = 0;
    for (int guard = 0; guard < 4096 && mcb_ok(m, s); guard++) {
        if (mcb_own(m, s) == 0 && mcb_size(m, s) > best) best = mcb_size(m, s);
        if (mcb_sig(m, s) != 'M') break;
        s += mcb_size(m, s) + 1;
    }
    return best;
}
static void mcb_split(Machine *m, uint16_t s, uint16_t paras) {
    uint16_t sz = mcb_size(m, s);
    if (sz > paras) { mcb_w(m, s + paras + 1, mcb_sig(m, s), 0, sz - paras - 1); mcb_w(m, s, 'M', mcb_own(m, s), paras); }
}
uint16_t dos_alloc(Machine *m, uint16_t paras, uint16_t *maxfree) {
    mcb_merge(m);
    uint16_t s = FIRST_MCB;
    for (int guard = 0; guard < 4096 && mcb_ok(m, s); guard++) {
        if (mcb_own(m, s) == 0 && mcb_size(m, s) >= paras) {
            mcb_split(m, s, paras); mcb_w(m, s, mcb_sig(m, s), PSP_SEG, paras); return s + 1;
        }
        if (mcb_sig(m, s) != 'M') break;
        s += mcb_size(m, s) + 1;
    }
    *maxfree = mcb_maxfree(m);
    return 0;
}
static int dos_free(Machine *m, uint16_t seg) {
    if (!mcb_ok(m, seg - 1)) return 0;
    mcb_w(m, seg - 1, mcb_sig(m, seg - 1), 0, mcb_size(m, seg - 1)); mcb_merge(m); return 1;
}
static int dos_resize(Machine *m, uint16_t seg, uint16_t paras, uint16_t *maxp) {
    uint16_t s = seg - 1;
    if (!mcb_ok(m, s)) { *maxp = 0; return 0; }
    while (mcb_sig(m, s) == 'M') {
        uint16_t n = s + mcb_size(m, s) + 1;
        if (!mcb_ok(m, n) || mcb_own(m, n) != 0) break;
        mcb_w(m, s, mcb_sig(m, n), mcb_own(m, s), mcb_size(m, s) + mcb_size(m, n) + 1);
    }
    if (mcb_size(m, s) < paras) { *maxp = mcb_size(m, s); return 0; }
    mcb_split(m, s, paras); return 1;
}

void dos_init(Machine *m) {
    mcb_w(m, FIRST_MCB, 'M', PSP_SEG, PSP_SEG - 1 - ENV_SEG);
    mcb_w(m, PSP_SEG - 1, 'Z', PSP_SEG, MEM_TOP - PSP_SEG);
    for (int i = 0; i < 5; i++) m->hkind[i] = 2;
    m->dta_seg = PSP_SEG; m->dta_off = 0x80;
    m->cwd[0] = 0;
}

/* ------------------------------------------------------------------ paths */
static int valid83(const char *n);
static void split83(const char *n, char *b, char *e);
typedef struct { char host[256]; char dos[13]; } Alias;
static int dir_aliases(const char *dir, Alias **out);
static void host_path(Machine *m, const char *dos, char *out, size_t outsz) {
    char p[300]; strncpy(p, dos, sizeof p - 1); p[sizeof p - 1] = 0;
    for (char *q = p; *q; q++) if (*q == '/') *q = '\\';
    char *s = p;
    if (s[0] && s[1] == ':') s += 2;
    char comps[32][64]; int nc = 0;
    if (*s != '\\') {   /* relative: start from cwd */
        char cw[256]; strcpy(cw, m->cwd);
        for (char *t = strtok(cw, "\\"); t && nc < 32; t = strtok(NULL, "\\")) strncpy(comps[nc++], t, 63);
    }
    char tmp[300]; strcpy(tmp, s);
    int first = 1;
    for (char *t = strtok(tmp, "\\"); t && nc < 32; t = strtok(NULL, "\\")) {
        if (first && *s == '\\' && !strcasecmp(t, "ST3")) { first = 0; continue; }
        first = 0;
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (nc) nc--; continue; }
        strncpy(comps[nc++], t, 63);
    }
    snprintf(out, outsz, "%s", m->cfg.root);
    for (int i = 0; i < nc; i++) {
        DIR *d = opendir(out); char found[256] = "";
        if (d) { struct dirent *e; while ((e = readdir(d))) if (!strcasecmp(e->d_name, comps[i])) { strcpy(found, e->d_name); break; } closedir(d); }
        if (!found[0] && strchr(comps[i], '~')) {   /* a short alias for a long host name (MYSONG~1.S3M) */
            Alias *al = NULL; int na = dir_aliases(out, &al);
            for (int k = 0; k < na; k++) if (!strcasecmp(al[k].dos, comps[i])) { strcpy(found, al[k].host); break; }
            free(al);
        }
        if (!found[0]) { for (int k = 0; comps[i][k]; k++) found[k] = toupper((unsigned char)comps[i][k]), found[k + 1] = 0; }
        size_t l = strlen(out); snprintf(out + l, outsz - l, "/%s", found);
    }
}

static int valid83(const char *n) {
    const char *dot = strchr(n, '.');
    if (!strcmp(n, ".") || !strcmp(n, "..")) return 1;
    if (n[0] == '.' || !n[0]) return 0;
    size_t b = dot ? (size_t)(dot - n) : strlen(n), e = dot ? strlen(dot + 1) : 0;
    if (b > 8 || e > 3 || (dot && strchr(dot + 1, '.'))) return 0;
    for (const char *q = n; *q; q++) if (*q == ' ' || (unsigned char)*q > 126) return 0;
    return 1;
}
static void split83(const char *n, char *b, char *e) {
    const char *dot = strrchr(n, '.');
    int i = 0;
    if (!strcmp(n, ".") || !strcmp(n, "..")) { strcpy(b, n); e[0] = 0; return; }
    for (const char *q = n; *q && q != dot && i < 8; q++) b[i++] = toupper((unsigned char)*q);
    b[i] = 0; i = 0;
    if (dot) for (const char *q = dot + 1; *q && i < 3; q++) e[i++] = toupper((unsigned char)*q);
    e[i] = 0;
}
/* Windows-95-style short names for host files whose names DOS can't hold: "My Song (final).s3m" -> MYSONG~1.S3M.
 * Derived per directory and deterministic: the long names, sorted, each take the first STEM~N that no 8.3 name or
 * earlier alias in that directory uses.  ST3 lists, loads and saves them like any other file. */
static void alias_clean(const char *s, size_t n, char *out, int max) {
    int k = 0;
    for (size_t i = 0; i < n && s[i] && k < max; i++) {
        int c = toupper((unsigned char)s[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("_-!#$%&'()@^`{}~", c))) out[k++] = (char)c;
    }
    out[k] = 0;
}
static int alias_cmp(const void *a, const void *b) { return strcmp(((const Alias *)a)->host, ((const Alias *)b)->host); }
static int dir_aliases(const char *dir, Alias **out) {
    *out = NULL;
    DIR *d = opendir(dir); if (!d) return 0;
    char (*taken)[13] = NULL; int nt = 0, ct = 0; Alias *al = NULL; int na = 0, ca = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (nm[0] == '.') continue;
        if (valid83(nm)) {
            char b[16], x[16]; split83(nm, b, x);
            if (nt == ct) { ct = ct ? ct * 2 : 64; taken = realloc(taken, ct * sizeof *taken); }
            snprintf(taken[nt++], 13, x[0] ? "%s.%s" : "%s", b, x);
        } else if (strlen(nm) < sizeof al->host) {
            if (na == ca) { ca = ca ? ca * 2 : 16; al = realloc(al, ca * sizeof *al); }
            snprintf(al[na].host, sizeof al->host, "%s", nm); al[na].dos[0] = 0; na++;
        }
    }
    closedir(d);
    if (na) qsort(al, na, sizeof *al, alias_cmp);
    for (int a = 0; a < na; a++) {
        const char *h = al[a].host, *dot = strrchr(h, '.');
        char b[64], x[8];
        alias_clean(h, dot && dot != h ? (size_t)(dot - h) : strlen(h), b, 63);
        alias_clean(dot && dot != h ? dot + 1 : "", 3, x, 3);
        if (!b[0]) strcpy(b, "FILE");
        for (int i = 1; i < 100000; i++) {
            char tail[8], cand[16]; snprintf(tail, sizeof tail, "~%d", i);
            int keep = 8 - (int)strlen(tail); if (keep > 6) keep = 6;
            snprintf(cand, sizeof cand, x[0] ? "%.*s%s.%s" : "%.*s%s", keep, b, tail, x);
            int used = 0;
            for (int k = 0; k < nt && !used; k++) used = !strcmp(taken[k], cand);
            for (int k = 0; k < a && !used; k++) used = !strcmp(al[k].dos, cand);
            if (!used) { snprintf(al[a].dos, sizeof al->dos, "%s", cand); break; }
        }
    }
    free(taken);
    *out = al; return na;
}

static int match_part(const char *n, const char *p, int len) {
    char pp[16]; int k = 0;
    for (int i = 0; p[i] && k < len; i++) { if (p[i] == '*') { while (k < len) pp[k++] = '?'; break; } pp[k++] = p[i]; }
    while (k < len) pp[k++] = ' ';
    char nn[16]; int nl = strlen(n);
    for (int i = 0; i < len; i++) nn[i] = i < nl ? n[i] : ' ';
    for (int i = 0; i < len; i++) if (pp[i] != '?' && pp[i] != nn[i]) return 0;
    return 1;
}
static int match83(const char *name, const char *pat) {
    char nb[16], ne[16], pb[16], pe[16];
    split83(name, nb, ne);
    char up[64]; int i = 0; for (; pat[i] && i < 63; i++) up[i] = toupper((unsigned char)pat[i]); up[i] = 0;
    if (!strcmp(up, "*.*") || !strcmp(up, "*")) return 1;
    const char *dot = strchr(up, '.');
    if (dot) { snprintf(pb, sizeof pb, "%.*s", (int)(dot - up), up); snprintf(pe, sizeof pe, "%s", dot + 1); }
    else { snprintf(pb, sizeof pb, "%s", up); pe[0] = 0; }
    return match_part(nb, pb, 8) && match_part(ne, pe, 3);
}

static void dos_time(time_t t, uint16_t *tm, uint16_t *dt) {
    struct tm *lt = localtime(&t);
    *tm = (lt->tm_hour << 11) | (lt->tm_min << 5) | (lt->tm_sec / 2);
    *dt = ((lt->tm_year - 80) << 9) | ((lt->tm_mon + 1) << 5) | lt->tm_mday;
}

static int fill_dta(Machine *m, const char *dir, const char *host, const char *name) {
    char full[900]; snprintf(full, sizeof full, "%s/%s", dir, host);
    struct stat st; if (stat(full, &st)) return 0;
    uint32_t a = ((uint32_t)m->dta_seg << 4) + m->dta_off;
    uint16_t t, d; dos_time(st.st_mtime, &t, &d);
    wb(m, a + 21, S_ISDIR(st.st_mode) ? 0x10 : 0x20); ww(m, a + 22, t); ww(m, a + 24, d);
    uint32_t sz = (uint32_t)st.st_size; ww(m, a + 26, sz); ww(m, a + 28, sz >> 16);
    char b[16], e[16]; split83(name, b, e);
    char dn[14]; if (e[0]) snprintf(dn, sizeof dn, "%s.%s", b, e); else snprintf(dn, sizeof dn, "%s", b);
    for (int i = 0; i < 13; i++) wb(m, a + 30 + i, i < (int)strlen(dn) ? dn[i] : 0);
    return 1;
}

static struct { char dir[512]; Alias *names; int n; } fslot[8];   /* .host = name on disk, .dos = name ST3 sees */
static int fslot_next;

static int find_next(Machine *m) {
    uint32_t a = ((uint32_t)m->dta_seg << 4) + m->dta_off;
    if (rb(m, a) != 0xA5) return 0;
    int slot = rb(m, a + 1) & 7; int pos = rw(m, a + 2);
    while (pos < fslot[slot].n) {
        int okk = fill_dta(m, fslot[slot].dir, fslot[slot].names[pos].host, fslot[slot].names[pos].dos);
        pos++; ww(m, a + 2, pos);
        if (okk) return 1;
    }
    return 0;
}

static int find_first(Machine *m, const char *dospat, uint16_t attr) {
    char hp[600]; host_path(m, dospat, hp, sizeof hp);
    char *sl = strrchr(hp, '/'); if (!sl) return 0;
    *sl = 0; const char *pat = sl + 1;
    /* pattern case: host_path may have uppercased; derive from DOS pattern instead */
    const char *dp = strrchr(dospat, '\\'); dp = dp ? dp + 1 : (dospat[0] && dospat[1] == ':' ? dospat + 2 : dospat);
    if (*dp) pat = dp;
    int slot = fslot_next++ & 7;
    free(fslot[slot].names); fslot[slot].names = NULL; fslot[slot].n = 0;
    snprintf(fslot[slot].dir, sizeof fslot[slot].dir, "%s", hp);
    DIR *d = opendir(hp); if (!d) return -1;
    int cap = 0; struct dirent *e;
    int atroot = !strcmp(hp, m->cfg.root);
    Alias *list = NULL; int n = 0;
    Alias *al = NULL; int na = -1;                /* aliases for long names, built on first need */
    while ((e = readdir(d))) {
        const char *nm = e->d_name, *dn = nm;
        if (!valid83(nm)) {
            if (nm[0] == '.') continue;
            if (na < 0) na = dir_aliases(hp, &al);
            dn = NULL;
            for (int k = 0; k < na; k++) if (!strcmp(al[k].host, nm)) { dn = al[k].dos; break; }
            if (!dn || !dn[0]) continue;
        }
        if ((!strcmp(nm, ".") || !strcmp(nm, "..")) && (atroot || !(attr & 0x10))) continue;
        char full[1200]; snprintf(full, sizeof full, "%s/%s", hp, nm);
        struct stat st; if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode) && !(attr & 0x10)) continue;
        if (!match83(dn, pat) && strcmp(nm, ".") && strcmp(nm, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; list = realloc(list, cap * sizeof *list); }
        snprintf(list[n].host, sizeof list->host, "%s", nm); snprintf(list[n].dos, sizeof list->dos, "%s", dn); n++;
    }
    closedir(d);
    free(al);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcasecmp(list[j - 1].dos, list[j].dos) > 0; j--) {   /* sort: . .. first */
        Alias t = list[j]; list[j] = list[j - 1]; list[j - 1] = t;
    }
    fslot[slot].names = list; fslot[slot].n = n;
    uint32_t a = ((uint32_t)m->dta_seg << 4) + m->dta_off;
    wb(m, a, 0xA5); wb(m, a + 1, slot); ww(m, a + 2, 0);
    LOGF(m, "FINDFIRST '%s' attr %02X -> %d\n", dospat, attr, n);
    return find_next(m);
}

/* ------------------------------------------------------------------ INT 21h */
static int newhandle(Machine *m) { for (int i = 5; i < MAX_HANDLES; i++) if (!m->hkind[i]) return i; return -1; }

static void con_write(Machine *m, const uint8_t *s, int n) {
    for (int i = 0; i < n; i++) bios_tty(m, s[i]);
    if (m->log) { fwrite(s, 1, n, m->log); fflush(m->log); }
}

int dos_service(Machine *m, uint16_t *fl) {
#define OK() do { *fl &= ~F_CF; return 1; } while (0)
#define ERR(code) do { set16(m, R_EAX, (code)); *fl |= F_CF; return 1; } while (0)
    char path[300], hp[700];
    switch (AH) {
    case 0x01: case 0x07: case 0x08:
        if (m->kq_head == m->kq_tail) return 2;
        setAL(m, m->keyq[m->kq_head] & 0xFF); m->kq_head = (m->kq_head + 1) % KEYQ;
        if (AH == 0x01) bios_tty(m, AL);
        return 1;
    case 0x02: { uint8_t c = DX & 0xFF; con_write(m, &c, 1); return 1; }
    case 0x06:
        if ((DX & 0xFF) != 0xFF) { uint8_t c = DX & 0xFF; con_write(m, &c, 1); return 1; }
        if (m->kq_head == m->kq_tail) { setAL(m, 0); *fl |= F_ZF; return 1; }
        setAL(m, m->keyq[m->kq_head] & 0xFF); m->kq_head = (m->kq_head + 1) % KEYQ; *fl &= ~F_ZF; return 1;
    case 0x09: {
        uint32_t a = ((uint32_t)DS << 4) + DX; uint8_t buf[4096]; int n = 0;
        while (n < 4096 && rb(m, a + n) != '$') { buf[n] = rb(m, a + n); n++; }
        con_write(m, buf, n); return 1; }
    case 0x0B: setAL(m, m->kq_head != m->kq_tail ? 0xFF : 0); return 1;
    case 0x0D: return 1;
    case 0x0E: setAL(m, 3); return 1;
    case 0x19: setAL(m, 2); return 1;
    case 0x1A: m->dta_seg = DS; m->dta_off = DX; return 1;
    case 0x25: ww(m, AL * 4, DX); ww(m, AL * 4 + 2, DS); return 1;
    case 0x2A: { time_t t = time(NULL); struct tm *lt = localtime(&t);
        set16(m, R_ECX, lt->tm_year + 1900); set16(m, R_EDX, ((lt->tm_mon + 1) << 8) | lt->tm_mday); setAL(m, lt->tm_wday); return 1; }
    case 0x2C: { time_t t = time(NULL); struct tm *lt = localtime(&t);
        set16(m, R_ECX, (lt->tm_hour << 8) | lt->tm_min); set16(m, R_EDX, lt->tm_sec << 8); return 1; }
    case 0x2F: m->cpu.seg[S_ES] = m->dta_seg; set16(m, R_EBX, m->dta_off); return 1;
    case 0x30: set16(m, R_EAX, 0x0005); set16(m, R_EBX, 0); set16(m, R_ECX, 0); return 1;
    case 0x33: if (AL == 0) set16(m, R_EDX, DX & 0xFF00); if (AL == 5) set16(m, R_EDX, (DX & 0xFF00) | 3); return 1;
    case 0x35: set16(m, R_EBX, rw(m, AL * 4)); m->cpu.seg[S_ES] = rw(m, AL * 4 + 2); return 1;
    case 0x36: set16(m, R_EAX, 8); set16(m, R_EBX, 0x7FFF); set16(m, R_ECX, 512); set16(m, R_EDX, 0xFFFF); return 1;
    case 0x39: asciiz(m, DS, DX, path, sizeof path); host_path(m, path, hp, sizeof hp);
        if (mkdir(hp, 0755)) ERR(3); OK();
    case 0x3A: asciiz(m, DS, DX, path, sizeof path); host_path(m, path, hp, sizeof hp);
        if (rmdir(hp)) ERR(3); OK();
    case 0x3B: {
        asciiz(m, DS, DX, path, sizeof path); host_path(m, path, hp, sizeof hp);
        struct stat st; if (stat(hp, &st) || !S_ISDIR(st.st_mode)) ERR(3);
        const char *rel = hp + strlen(m->cfg.root); while (*rel == '/') rel++;
        snprintf(m->cwd, sizeof m->cwd, "%s", rel);
        for (char *q = m->cwd; *q; q++) { if (*q == '/') *q = '\\'; *q = toupper((unsigned char)*q); }
        OK(); }
    case 0x3C: case 0x5B: {
        asciiz(m, DS, DX, path, sizeof path); host_path(m, path, hp, sizeof hp);
        if (AH == 0x5B && access(hp, F_OK) == 0) ERR(80);
        int h = newhandle(m); if (h < 0) ERR(4);
        FILE *f = fopen(hp, "w+b"); if (!f) ERR(3);
        m->handles[h] = f; m->hkind[h] = 1; LOGF(m, "CREATE '%s' -> %d\n", path, h);
        set16(m, R_EAX, h); OK(); }
    case 0x3D: {
        asciiz(m, DS, DX, path, sizeof path);
        if (!strcasecmp(path, "EMMXXXX0") || strstr(path, "EMMXXXX0")) {          /* the EMS driver's device */
            if (!m->ems.store) { LOGF(m, "OPEN %s (EMS probe) -> not found\n", path); ERR(2); }
            int h = newhandle(m); if (h < 0) ERR(4);
            m->hkind[h] = 3; LOGF(m, "OPEN %s (EMS probe) -> %d\n", path, h);
            set16(m, R_EAX, h); OK(); }
        host_path(m, path, hp, sizeof hp);
        struct stat st; if (stat(hp, &st) || S_ISDIR(st.st_mode)) { LOGF(m, "OPEN '%s' -> not found\n", path); ERR(2); }
        int h = newhandle(m); if (h < 0) ERR(4);
        FILE *f = fopen(hp, (AL & 3) ? "r+b" : "rb");
        if (!f && (AL & 3)) f = fopen(hp, "rb");
        if (!f) ERR(5);
        m->handles[h] = f; m->hkind[h] = 1; LOGF(m, "OPEN '%s' mode %02X -> %d\n", path, AL, h);
        set16(m, R_EAX, h); OK(); }
    case 0x3E:
        if (BX >= MAX_HANDLES || !m->hkind[BX]) ERR(6);
        if (m->hkind[BX] == 1) fclose(m->handles[BX]);
        if (BX >= 5) { m->hkind[BX] = 0; m->handles[BX] = NULL; }
        OK();
    case 0x3F: {
        if (BX >= MAX_HANDLES || !m->hkind[BX]) ERR(6);
        if (m->hkind[BX] >= 2) { set16(m, R_EAX, 0); OK(); }
        uint8_t *buf = malloc(CX ? CX : 1);
        size_t n = fread(buf, 1, CX, m->handles[BX]);
        for (size_t i = 0; i < n; i++) wb(m, ((uint32_t)DS << 4) + (uint16_t)(DX + i), buf[i]);
        free(buf); set16(m, R_EAX, n); OK(); }
    case 0x40: {
        if (BX >= MAX_HANDLES || !m->hkind[BX]) ERR(6);
        uint8_t *buf = malloc(CX ? CX : 1);
        for (int i = 0; i < CX; i++) buf[i] = rb(m, ((uint32_t)DS << 4) + (uint16_t)(DX + i));
        if (m->hkind[BX] == 2) { con_write(m, buf, CX); free(buf); set16(m, R_EAX, CX); OK(); }
        if (m->hkind[BX] == 3) { free(buf); set16(m, R_EAX, CX); OK(); }
        if (CX == 0) { fflush(m->handles[BX]); if (ftruncate(fileno(m->handles[BX]), ftell(m->handles[BX]))) {} free(buf); set16(m, R_EAX, 0); OK(); }
        size_t n = fwrite(buf, 1, CX, m->handles[BX]); free(buf); set16(m, R_EAX, n); OK(); }
    case 0x41: asciiz(m, DS, DX, path, sizeof path); host_path(m, path, hp, sizeof hp);
        if (unlink(hp)) ERR(2); OK();
    case 0x42: {
        if (BX >= MAX_HANDLES || m->hkind[BX] != 1) ERR(6);
        int32_t off = (int32_t)(((uint32_t)CX << 16) | DX);
        if (fseek(m->handles[BX], off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END)) ERR(25);
        long pos = ftell(m->handles[BX]);
        set16(m, R_EAX, pos & 0xFFFF); set16(m, R_EDX, (pos >> 16) & 0xFFFF); OK(); }
    case 0x43: {
        asciiz(m, DS, DX, path, sizeof path); host_path(m, path, hp, sizeof hp);
        struct stat st; if (stat(hp, &st)) ERR(2);
        if (AL == 0) set16(m, R_ECX, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
        OK(); }
    case 0x44:
        switch (AL) {
        case 0x00: if (BX >= MAX_HANDLES || !m->hkind[BX]) ERR(6);
            set16(m, R_EDX, m->hkind[BX] == 2 ? 0x80D3 : m->hkind[BX] == 3 ? 0xC080 : 0x0002); OK();
        case 0x01: OK();
        case 0x06: case 0x07: setAL(m, 0xFF); OK();
        case 0x08: set16(m, R_EAX, 1); OK();
        case 0x09: set16(m, R_EDX, 0); OK();
        default: ERR(1);
        }
    case 0x45: {
        if (BX >= MAX_HANDLES || !m->hkind[BX]) ERR(6);
        int h = newhandle(m); if (h < 0) ERR(4);
        if (m->hkind[BX] == 1) { m->handles[h] = fdopen(dup(fileno(m->handles[BX])), "r+b"); if (!m->handles[h]) ERR(4); }
        m->hkind[h] = m->hkind[BX]; set16(m, R_EAX, h); OK(); }
    case 0x47: {
        char s[80]; snprintf(s, sizeof s, "ST3%s%s", m->cwd[0] ? "\\" : "", m->cwd);
        for (int i = 0; i <= (int)strlen(s); i++) wb(m, ((uint32_t)DS << 4) + (uint16_t)(SI + i), toupper((unsigned char)s[i]));
        OK(); }
    case 0x48: {
        uint16_t mx = 0, seg = dos_alloc(m, BX, &mx);
        LOGF(m, "ALLOC %04X -> %04X (max %04X)\n", BX, seg, mx);
        if (!seg) { set16(m, R_EBX, mx); ERR(8); }
        set16(m, R_EAX, seg); OK(); }
    case 0x49: if (!dos_free(m, ES)) ERR(9); OK();
    case 0x4A: {
        uint16_t mx = 0;
        if (!dos_resize(m, ES, BX, &mx)) { LOGF(m, "RESIZE %04X to %04X FAIL max %04X\n", ES, BX, mx); set16(m, R_EBX, mx); ERR(8); }
        OK(); }
    case 0x4B: LOGF(m, "EXEC requested -> fail\n"); ERR(2);
    case 0x4C: m->exited = 1; m->exit_code = AL; return 1;
    case 0x4D: set16(m, R_EAX, 0); return 1;
    case 0x4E: {
        asciiz(m, DS, DX, path, sizeof path);
        int r = find_first(m, path, CX);
        if (r < 0) ERR(3);
        if (!r) ERR(18);
        OK(); }
    case 0x4F: if (!find_next(m)) ERR(18); OK();
    case 0x50: return 1;
    case 0x51: case 0x62: set16(m, R_EBX, PSP_SEG); return 1;
    case 0x54: setAL(m, 0); return 1;
    case 0x56: {
        char p2[300], hp2[700];
        asciiz(m, DS, DX, path, sizeof path); asciiz(m, ES, DI, p2, sizeof p2);
        host_path(m, path, hp, sizeof hp); host_path(m, p2, hp2, sizeof hp2);
        if (rename(hp, hp2)) ERR(2); OK(); }
    case 0x57:
        if (AL == 0) {
            if (BX >= MAX_HANDLES || m->hkind[BX] != 1) ERR(6);
            struct stat st; fstat(fileno(m->handles[BX]), &st);
            uint16_t t, d; dos_time(st.st_mtime, &t, &d); set16(m, R_ECX, t); set16(m, R_EDX, d);
        }
        OK();
    case 0x58: set16(m, R_EAX, 0); OK();
    case 0x59: set16(m, R_EAX, 0); set16(m, R_EBX, 0); set16(m, R_ECX, 0); return 1;
    }
    return 0;
#undef OK
#undef ERR
}
