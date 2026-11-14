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
#include <time.h>
#include <sys/ioctl.h>

#define OUT_RATE 48000

static double mono_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

double audio_queued_ms(Audio *a) {
    int n = 0;
    if (a->fd < 0 || ioctl(a->fd, FIONREAD, &n) < 0) return 0;
    return n / (OUT_RATE * 4 / 1000.0);
}
int audio_active(Audio *a) { return a->fd >= 0 && a->on && mono_now() - a->last_feed < 0.2; }

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
    int off = 0, bytes = a->on_n * (int)sizeof a->obuf[0];   /* on_n counts samples, write() wants bytes */
    while (off < bytes) {
        ssize_t w = write(a->fd, (char *)a->obuf + off, bytes - off);
        if (w <= 0) { if (errno == EAGAIN) a->dropped += bytes - off; break; }
        off += w; a->written += w;
    }
    a->on_n = 0;
}

/* resample one input frame (linear interpolation) into the output buffer */
static void push_frame(Audio *a, float l, float r, double step) {
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

/* feed DAC bytes (8-bit unsigned; stereo = L,R interleaved) at input frame rate. The emulator hands over a byte
 * or two at a time, so a stereo frame may be split across calls: keep the partial frame in a->part. */
void audio_feed(Audio *a, const uint8_t *d, int n, double rate, int stereo) {
    int ch = stereo ? 2 : 1;
    double step = rate / OUT_RATE;
    a->last_feed = mono_now(); a->in_bytes += n; a->in_rate = rate; a->in_stereo = stereo;
    if (stereo != a->in_stereo_prev) { a->part_n = 0; a->in_stereo_prev = stereo; }
    for (int i = 0; i < n; i++) {
        a->part[a->part_n++] = d[i];
        if (a->part_n < ch) continue;
        a->part_n = 0;
        float l = (a->part[0] - 128) / 128.0f, r = stereo ? (a->part[1] - 128) / 128.0f : l;
        push_frame(a, l, r, step);
    }
    flush_out(a);
}
