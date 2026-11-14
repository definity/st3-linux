#define _GNU_SOURCE
/* audio.c — virtual SB DAC stream (8-bit unsigned, any rate, mono/stereo) -> 48 kHz s16 stereo -> pw-play pipe.
 * Non-blocking: if the pipe is full we drop (never stall the emulator). Optional WAV tee for debugging. */
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#define OUT_RATE 48000

int audio_open(Audio *a, const char *cmd) {
    memset(a, 0, sizeof *a);
    a->fd = -1; a->volume = 1.0f; a->on = 1;
    if (!cmd || !*cmd) return -1;
    int p[2];
    if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[0], 0); close(p[0]); close(p[1]);
        int nul = open("/dev/null", O_WRONLY); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    a->fd = p[1]; a->pid = pid;
    fcntl(a->fd, F_SETFL, O_NONBLOCK);
    fcntl(a->fd, F_SETPIPE_SZ, 32768);   /* ~170 ms at 48k s16 stereo: bounds latency */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void audio_close(Audio *a) {
    if (a->fd >= 0) { close(a->fd); a->fd = -1; }
    if (a->pid > 0) { kill(a->pid, SIGTERM); waitpid(a->pid, NULL, 0); a->pid = 0; }
}

static void flush_out(Audio *a) {
    if (a->fd < 0 || !a->on) { a->on_n = 0; return; }
    int off = 0;
    while (off < a->on_n) {
        ssize_t w = write(a->fd, (char *)a->obuf + off, a->on_n - off);
        if (w <= 0) { if (errno == EAGAIN) a->dropped += a->on_n - off; break; }
        off += w;
    }
    a->on_n = 0;
}

/* feed DAC bytes at input rate; resample with linear interpolation */
void audio_feed(Audio *a, const uint8_t *d, int n, double rate, int stereo) {
    int ch = stereo ? 2 : 1, frames = n / ch;
    double step = rate / OUT_RATE;
    for (int i = 0; i < frames; i++) {
        float l = (d[i * ch] - 128) / 128.0f, r = stereo ? (d[i * ch + 1] - 128) / 128.0f : l;
        /* emit output samples whose time falls between previous input frame and this one */
        while (a->phase <= 1.0) {
            float ol = a->pl + (l - a->pl) * (float)a->phase, orr = a->pr + (r - a->pr) * (float)a->phase;
            int sl = (int)(ol * 30000.0f * a->volume), sr = (int)(orr * 30000.0f * a->volume);
            if (sl > 32767) sl = 32767; if (sl < -32768) sl = -32768;
            if (sr > 32767) sr = 32767; if (sr < -32768) sr = -32768;
            a->obuf[a->on_n++] = (int16_t)sl; a->obuf[a->on_n++] = (int16_t)sr;
            if (a->on_n >= (int)(sizeof a->obuf / sizeof a->obuf[0])) flush_out(a);
            a->phase += step;
        }
        a->phase -= 1.0;
        a->pl = l; a->pr = r;
    }
    flush_out(a);
}
