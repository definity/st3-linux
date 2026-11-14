/* keys.h — BIOS keycodes (scan<<8 | ascii) and a small key-script parser shared by tests and the terminal UI. */
#ifndef ST3_KEYS_H
#define ST3_KEYS_H
#include <stdint.h>

#define K_ESC   0x011B
#define K_ENTER 0x1C0D
#define K_TAB   0x0F09
#define K_STAB  0x0F00
#define K_BS    0x0E08
#define K_UP    0x4800
#define K_DOWN  0x5000
#define K_LEFT  0x4B00
#define K_RIGHT 0x4D00
#define K_PGUP  0x4900
#define K_PGDN  0x5100
#define K_HOME  0x4700
#define K_END   0x4F00
#define K_INS   0x5200
#define K_DEL   0x5300
#define K_SPACE 0x3920

uint16_t key_ascii(int ch);              /* printable ASCII / control char -> BIOS key */
uint16_t key_fn(int n, int mod);         /* F1..F12, mod: 0 none, 1 shift, 2 ctrl, 3 alt */
uint16_t key_alt(int ch);                /* Alt+letter/digit */
uint16_t key_ctrl_special(uint16_t k);   /* Ctrl+arrow/home/end/pgup/pgdn */
uint16_t key_shift_special(uint16_t k);

typedef struct { uint16_t key; int wait_ms; } KeyTok;
typedef struct { KeyTok tok[4096]; int n, pos; } KeyScript;
int keyscript_parse(KeyScript *ks, const char *s);
#endif
