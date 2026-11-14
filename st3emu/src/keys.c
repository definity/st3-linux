/* keys.c — scancode tables (US layout, PC set 1 as reported by BIOS INT 16h) and key-script parsing. */
#include "keys.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static uint8_t sc_of(int ch) {
    static const char *rows[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "\\zxcvbnm,./"};
    static const uint8_t base[] = {0x02, 0x10, 0x1E, 0x2B};
    static const char *shifted = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    static const char *plain = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
    const char *p = strchr(shifted, ch);
    if (ch && p) ch = plain[p - shifted];
    for (int r = 0; r < 4; r++) { const char *q = strchr(rows[r], ch); if (ch && q) return base[r] + (q - rows[r]); }
    return 0;
}

uint16_t key_ascii(int ch) {
    if (ch == ' ') return K_SPACE;
    if (ch == '\r' || ch == '\n') return K_ENTER;
    if (ch == '\t') return K_TAB;
    if (ch == 0x1B) return K_ESC;
    if (ch == 0x7F || ch == 0x08) return K_BS;
    if (ch >= 1 && ch <= 26) return (sc_of('a' + ch - 1) << 8) | ch;   /* Ctrl+letter */
    return (sc_of(ch) << 8) | (ch & 0xFF);
}

uint16_t key_fn(int n, int mod) {
    if (n >= 1 && n <= 10) {
        static const uint8_t base[4] = {0x3B, 0x54, 0x5E, 0x68};
        return (uint16_t)((base[mod & 3] + n - 1) << 8);
    }
    if (n == 11) { static const uint8_t b[4] = {0x85, 0x87, 0x89, 0x8B}; return (uint16_t)(b[mod & 3] << 8); }
    if (n == 12) { static const uint8_t b[4] = {0x86, 0x88, 0x8A, 0x8C}; return (uint16_t)(b[mod & 3] << 8); }
    return 0;
}

uint16_t key_alt(int ch) {
    ch = tolower(ch);
    if (ch >= '1' && ch <= '9') return (uint16_t)((0x78 + ch - '1') << 8);
    if (ch == '0') return 0x8100;
    if (ch == '-') return 0x8200;
    if (ch == '=') return 0x8300;
    return (uint16_t)(sc_of(ch) << 8);
}

uint16_t key_ctrl_special(uint16_t k) {
    switch (k) {
    case K_LEFT: return 0x7300; case K_RIGHT: return 0x7400; case K_END: return 0x7500; case K_PGDN: return 0x7600;
    case K_HOME: return 0x7700; case K_PGUP: return 0x8400; case K_UP: return 0x8D00; case K_DOWN: return 0x9100;
    case K_INS: return 0x9200; case K_DEL: return 0x9300;
    }
    return k;
}
uint16_t key_shift_special(uint16_t k) { return k; }   /* BIOS reports same codes; shift state via INT16/02 */

int keyscript_parse(KeyScript *ks, const char *s) {
    ks->n = ks->pos = 0;
    while (*s && ks->n < 4096) {
        KeyTok t = {0, 0};
        if (*s == '{') {
            const char *e = strchr(s, '}');
            if (!e) break;
            char tk[32]; int l = e - s - 1; if (l > 31) l = 31; memcpy(tk, s + 1, l); tk[l] = 0; s = e + 1;
            if (tk[0] == 'W' && (isdigit((unsigned char)tk[1]) || !tk[1])) t.wait_ms = tk[1] ? atoi(tk + 1) : 300;
            else if (!strncmp(tk, "ALT-", 4)) t.key = key_alt(tk[4]);
            else if (!strncmp(tk, "CTRL-", 5)) t.key = key_ascii(tolower((unsigned char)tk[5]) - 'a' + 1);
            else if (!strncmp(tk, "SHIFT-F", 7)) t.key = key_fn(atoi(tk + 7), 1);
            else if (tk[0] == 'F' && isdigit((unsigned char)tk[1])) t.key = key_fn(atoi(tk + 1), 0);
            else {
                static const struct { const char *n; uint16_t k; } T[] = {
                    {"ESC", K_ESC}, {"ENTER", K_ENTER}, {"TAB", K_TAB}, {"BS", K_BS}, {"UP", K_UP}, {"DOWN", K_DOWN},
                    {"LEFT", K_LEFT}, {"RIGHT", K_RIGHT}, {"PGUP", K_PGUP}, {"PGDN", K_PGDN}, {"HOME", K_HOME},
                    {"END", K_END}, {"INS", K_INS}, {"DEL", K_DEL}, {"SPACE", K_SPACE}};
                for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(tk, T[i].n)) t.key = T[i].k;
            }
        } else t.key = key_ascii((unsigned char)*s++);
        ks->tok[ks->n++] = t;
    }
    return ks->n;
}
