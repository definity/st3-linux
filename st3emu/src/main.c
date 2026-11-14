/* main.c — st3: Scream Tracker 3.21 (Future Crew, 1994) running in your Linux terminal.
 * An emulated 386/VGA/SoundBlaster-Pro PC with BIOS/DOS HLE runs the original (unpacked) ST3.EXE. */
#include "machine.h"
#include "render.h"
#include "audio.h"
#include "keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>

extern const unsigned char st3_exe[];
extern const unsigned int st3_exe_len;

static uint8_t MEM[MEM_SIZE];
static Machine M;
static Render R;
static Audio A;
static struct termios tio_saved;
static int tio_ok;
static volatile sig_atomic_t winch = 1, want_quit;
static int term_rows = 24, term_cols = 80;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void term_restore(void) {
    if (!tio_ok) return;
    const char *s = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l";
    if (write(1, s, strlen(s)) < 0) {}
    tcsetattr(0, TCSAFLUSH, &tio_saved);
    tio_ok = 0;
}
static void on_signal(int sig) {
    if (sig == SIGWINCH) { winch = 1; return; }
    if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) { want_quit = 1; return; }
    term_restore(); audio_close(&A);
    signal(sig, SIG_DFL); raise(sig);
}
static int term_setup(void) {
    if (!isatty(0) || !isatty(1)) { fprintf(stderr, "st3: needs a terminal (stdin/stdout must be a tty)\n"); return -1; }
    if (tcgetattr(0, &tio_saved)) return -1;
    struct termios t = tio_saved;
    t.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR | BRKINT | ISTRIP | PARMRK);
    t.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_oflag &= ~OPOST;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    tio_ok = 1;
    atexit(term_restore);
    const char *s = "\x1b[?1049h\x1b[?25l\x1b[2J";
    if (write(1, s, strlen(s)) < 0) {}
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_signal;
    sigaction(SIGWINCH, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGABRT, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    return 0;
}
static void write_all(const char *b, int n) {
    while (n > 0) { ssize_t w = write(1, b, n); if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return; } b += w; n -= w; }
}

/* ------------------------------------------------------------------ keyboard */
static uint8_t ibuf[256]; static int ilen; static double esc_t;
static double shift_clear_at;

static void push_key(uint16_t k, int shift) {
    if (!k) return;
    machine_key(&M, k);
    if (shift) { M.shift_flags = shift; shift_clear_at = now() + 0.25; }
}
/* xterm modifier param m: 1 + (shift?1) + (alt?2) + (ctrl?4) */
static int mod_shift_flags(int m) {
    int f = 0, b = m - 1; if (b < 0) b = 0;
    if (b & 1) f |= 0x02; if (b & 4) f |= 0x04; if (b & 2) f |= 0x08;
    return f;
}
static int fnmod(int m) { int b = m > 1 ? m - 1 : 0; return (b & 4) ? 2 : (b & 2) ? 3 : (b & 1) ? 1 : 0; }

/* parse as many complete sequences from ibuf as possible */
static void parse_input(int flush_esc) {
    int i = 0;
    while (i < ilen) {
        uint8_t c = ibuf[i];
        if (c == 0x1D) { want_quit = 1; i++; continue; }            /* Ctrl-] : leave emulator */
        if (c != 0x1B) {
            if (c >= 0x80) { i++; continue; }                         /* ignore non-ASCII input */
            push_key(key_ascii(c), 0); i++; continue;
        }
        if (i + 1 >= ilen) { if (flush_esc) { push_key(K_ESC, 0); i++; } break; }
        uint8_t c1 = ibuf[i + 1];
        if (c1 == '[' || c1 == 'O') {
            int j = i + 2, p[4] = {0, 0, 0, 0}, np = 0;
            while (j < ilen && ((ibuf[j] >= '0' && ibuf[j] <= '9') || ibuf[j] == ';')) {
                if (ibuf[j] == ';') { if (np < 3) np++; } else p[np] = p[np] * 10 + (ibuf[j] - '0');
                j++;
            }
            if (j >= ilen) { if (flush_esc) { i = ilen; } break; }    /* incomplete */
            uint8_t fin = ibuf[j]; int mod = np >= 1 ? p[1] : (c1 == 'O' && p[0] > 1 ? p[0] : 1);
            int sh = mod_shift_flags(mod);
            uint16_t k = 0;
            switch (fin) {
            case 'A': k = K_UP; break; case 'B': k = K_DOWN; break; case 'C': k = K_RIGHT; break; case 'D': k = K_LEFT; break;
            case 'H': k = K_HOME; break; case 'F': k = K_END; break;
            case 'Z': k = K_STAB; sh |= 0x02; break;
            case 'P': k = key_fn(1, fnmod(mod)); break; case 'Q': k = key_fn(2, fnmod(mod)); break;
            case 'R': k = key_fn(3, fnmod(mod)); break; case 'S': k = key_fn(4, fnmod(mod)); break;
            case '~':
                switch (p[0]) {
                case 1: case 7: k = K_HOME; break; case 2: k = K_INS; break; case 3: k = K_DEL; break;
                case 4: case 8: k = K_END; break; case 5: k = K_PGUP; break; case 6: k = K_PGDN; break;
                case 11: case 12: case 13: case 14: case 15: k = key_fn(p[0] - 10, fnmod(mod)); break;
                case 17: case 18: case 19: case 20: case 21: k = key_fn(p[0] - 11, fnmod(mod)); break;
                case 23: case 24: k = key_fn(p[0] - 12, fnmod(mod)); break;
                }
                break;
            }
            if (k && (sh & 0x04) && (fin != '~' || p[0] < 11) && !(fin >= 'P' && fin <= 'S')) k = key_ctrl_special(k);
            push_key(k, sh);
            i = j + 1; continue;
        }
        /* ESC + char = Alt+char */
        if (c1 >= 0x20 && c1 < 0x7F) { push_key(key_alt(c1), 0x08); i += 2; continue; }
        if (c1 == 0x1B) { push_key(K_ESC, 0); i++; continue; }
        push_key(K_ESC, 0); i++;
    }
    memmove(ibuf, ibuf + i, ilen - i); ilen -= i;
    if (ilen && ibuf[0] == 0x1B) { if (!esc_t) esc_t = now(); } else esc_t = 0;
}

static void get_winsize(void) {
    struct winsize ws;
    if (!ioctl(1, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col) { term_rows = ws.ws_row; term_cols = ws.ws_col; }
}

static void pcm_sink(void *ud, const uint8_t *d, int n, double rate, int stereo) { audio_feed((Audio *)ud, d, n, rate, stereo); }

static void usage(void) {
    fprintf(stderr,
        "st3 — Scream Tracker 3.21 (Future Crew, 1994) in your terminal\n"
        "usage: st3 [options] [-- ST3-switches]\n"
        "  --root DIR      directory used as C:\\ST3 (songs, samples, ST3.SET); default: current directory\n"
        "  --no-sound      disable audio output\n"
        "  --audio CMD     audio sink command reading s16le 48k stereo on stdin\n"
        "                  default: \"pw-play -a --format s16 --rate 48000 --channels 2 --latency 60ms -\"\n"
        "  --mips N        emulated CPU speed in million instructions/s (default 25)\n"
        "  --exe FILE      run another (unpacked) ST3 executable instead of the built-in one\n"
        "  --glyphs MODE   st3 (default) | blocks\n"
        "  --log FILE      write emulator debug log\n"
        "keys: as in DOS ST3 (F1..F10, Shift-F9 setup, Ctrl-L load, F5 play, F8 stop, Ctrl-Q quit).\n"
        "      Ctrl-] leaves the emulator immediately. Terminal must be at least 80x50.\n");
}

int main(int argc, char **argv) {
    MachineConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.mips = 25.0;
    static char root[4096], args[256];
    if (!getcwd(root, sizeof root)) strcpy(root, ".");
    const char *audio_cmd = "pw-play -a --format s16 --rate 48000 --channels 2 --latency 60ms -";
    const char *exe = NULL, *logp = NULL; int glyph_mode = 0, nosound = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) { if (!realpath(argv[++i], root)) { fprintf(stderr, "st3: bad --root\n"); return 1; } }
        else if (!strcmp(argv[i], "--no-sound")) nosound = 1;
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio_cmd = argv[++i];
        else if (!strcmp(argv[i], "--mips") && i + 1 < argc) cfg.mips = atof(argv[++i]);
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--glyphs") && i + 1 < argc) { i++; glyph_mode = !strcmp(argv[i], "blocks"); }
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
        else if (!strcmp(argv[i], "--")) { for (int k = i + 1; k < argc; k++) { strcat(args, k > i + 1 ? " " : ""); strncat(args, argv[k], 100); } break; }
        else { usage(); return argv[i][1] == 'h' || !strcmp(argv[i], "--help") ? 0 : 1; }
    }
    cfg.root = root; cfg.args = args;
    if (cfg.mips < 1) cfg.mips = 1;
    FILE *log = logp ? fopen(logp, "w") : NULL;
    machine_init(&M, MEM, &cfg, log);
    int lr = exe ? machine_load_exe(&M, exe) : machine_load_exe_mem(&M, st3_exe, st3_exe_len, "ST3.EXE");
    if (lr) { fprintf(stderr, "st3: cannot load executable (%d)\n", lr); return 1; }
    if (!nosound && audio_open(&A, audio_cmd) == 0) { M.pcm_sink = pcm_sink; M.pcm_ud = &A; }
    if (term_setup()) return 1;
    render_init(&R);

    double t0 = now(), t0_orig = t0, dropped_total = 0, last_render = 0; int font_seen = -1, charh_seen = -1;
    while (!M.exited && !want_quit) {
        double wall = now() - t0, vt = machine_vtime(&M);
        if (vt < wall) {
            double target = wall < vt + 0.02 ? wall : vt + 0.02;
            uint64_t n = (uint64_t)((target - vt) * cfg.mips * 1e6) + 1;
            if (machine_run(&M, n) < 0) break;
        }
        vt = machine_vtime(&M);
        if (wall - vt > 0.25) { dropped_total += (wall - vt) - 0.05; t0 += (wall - vt) - 0.05; }   /* host too slow: drop time */
        if (shift_clear_at && now() > shift_clear_at && M.kq_head == M.kq_tail) { M.shift_flags = 0; shift_clear_at = 0; }

        if (winch) { winch = 0; get_winsize(); R.force = 1; write_all("\x1b[0m\x1b[2J", 8); }
        if (wall - last_render >= 1.0 / 30 && !M.fontmode) {
            int charh = M.mem[0x485] ? M.mem[0x485] : 16;
            if (M.font_uploads != font_seen || charh != charh_seen) {
                render_build_map(&R, M.font, charh > 16 ? 16 : charh, glyph_mode);
                font_seen = M.font_uploads; charh_seen = charh; R.force = 1;
            }
            int rows = M.mem[0x484] + 1;
            int y0 = term_rows > rows ? (term_rows - rows) / 2 : 0, x0 = term_cols > 80 ? (term_cols - 80) / 2 : 0;
            int n = render_frame(&R, M.mem + 0xB8000, rows, M.dac, M.attr, M.seq[3], term_rows, term_cols, y0, x0);
            if (n) write_all(R.out, n);
            if (term_rows < rows || term_cols < 80) {   /* too small: say so on the last line, every frame */
                char txt[160], msg[220];
                snprintf(txt, sizeof txt, " ST3 needs 80x%d (now %dx%d): enlarge window or zoom out (Ctrl -) ", rows, term_cols, term_rows);
                if ((int)strlen(txt) > term_cols - 1) txt[term_cols > 1 ? term_cols - 1 : 0] = 0;
                int l = snprintf(msg, sizeof msg, "\x1b[%d;1H\x1b[0;30;43m%s\x1b[0m", term_rows, txt);
                write_all(msg, l);
                for (int x = 0; x < 80; x++) R.prev[term_rows - 1 < 50 ? term_rows - 1 : 49][x].u = 0xFFFFFFFF;
            }
            last_render = wall;
        }
        /* input: wait a little (keeps CPU use sane) */
        fd_set fs; FD_ZERO(&fs); FD_SET(0, &fs);
        struct timeval tv = {0, 2000};
        if (select(1, &fs, NULL, NULL, &tv) > 0) {
            ssize_t r = read(0, ibuf + ilen, sizeof ibuf - ilen);
            if (r > 0) ilen += r;
        }
        if (ilen) parse_input(esc_t && now() - esc_t > 0.03);
    }
    LOGF(&M, "exit: vtime %.2f wall %.2f (time dropped %.2f) audio dropped %ld bytes, insns %llu\n",
         machine_vtime(&M), now() - t0_orig, dropped_total, A.dropped, (unsigned long long)M.cpu.icount);
    term_restore();
    audio_close(&A);
    if (M.exited) fprintf(stderr, "st3: ST3 exited (code %d)\n", M.exit_code);
    if (log) fclose(log);
    return 0;
}
