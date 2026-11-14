/* st3patch.c — the one change st3 makes to ST3 itself: a line about the Ctrl-F function-key prefix (main.c) in the
 * "Quickhelp; GLOBAL KEYS" page (F10 on the Info Screen).  Applied in memory after loading, only when the bytes match
 * ST3 3.21 exactly; the .EXE is never modified.
 *   ST3 shows that page with  push 20h / push ds / push 126Eh  (image offset 50C7h), DS = load segment + 3A94h.
 *   The text has no spare bytes, so a copy with the extra line goes to DS:C000, inside DS:90A0..FFFF which ST3 never
 *   uses (checked: still all zero after loading, playing, editing, every help page, setup, save prompt, library). */
#include "machine.h"
#include <string.h>

#define IMG        ((uint32_t)(PSP_SEG + 0x10) * 16)
#define CODE_OFF   0x50C7u
#define DS_OFF     0x3A940u
#define OLD_STR    0x126Eu
#define NEW_STR    0xC000u

static const char AFTER[] = "CTRL-T      Go to infopage & track\n";
static const char LINE[]  = "CTRL-F 1..0 Same as F1..F10 (SHIFT: SF1..SF10)\n";

int st3_patch_help(Machine *m) {
    static const uint8_t code[6] = {0x6A, 0x20, 0x1E, 0x68, OLD_STR & 0xFF, OLD_STR >> 8};
    uint8_t *c = m->mem + IMG + CODE_OFF, *ds = m->mem + IMG + DS_OFF;
    const char *old = (const char *)ds + OLD_STR;
    if (memcmp(c, code, 6) || strncmp(old, "Quickhelp; GLOBAL KEYS\n", 23)) return -1;
    size_t n = strlen(old);
    const char *at = strstr(old, AFTER);
    if (!at || n + sizeof LINE > 0x1000) return -1;
    for (size_t i = 0; i < 0x1000; i++) if (ds[NEW_STR + i]) return -1;
    char *dst = (char *)ds + NEW_STR;
    size_t head = (size_t)(at - old) + strlen(AFTER);
    memcpy(dst, old, head);
    memcpy(dst + head, LINE, strlen(LINE));
    memcpy(dst + head + strlen(LINE), old + head, n - head + 1);
    c[4] = NEW_STR & 0xFF; c[5] = NEW_STR >> 8;
    return 0;
}
