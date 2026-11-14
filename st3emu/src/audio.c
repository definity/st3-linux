#define _GNU_SOURCE
/* audio.c — mixed emulator output (48 kHz s16 stereo, from mixer.c) -> sink command's stdin (pw-play).
 * Non-blocking: if the pipe is full we drop (never stall the emulator). */
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
int audio_active(Audio *a) { return a->fd >= 0 && mono_now() - a->last_write < 0.2; }

int audio_open(Audio *a, const char *cmd) {
    memset(a, 0, sizeof *a);
    a->fd = -1;
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

void audio_write(Audio *a, const int16_t *frames, int nframes) {
    if (a->fd < 0) return;
    a->last_write = mono_now();
    int off = 0, bytes = nframes * 4;
    while (off < bytes) {
        ssize_t w = write(a->fd, (const char *)frames + off, bytes - off);
        if (w <= 0) { if (errno == EAGAIN) a->dropped += bytes - off; break; }
        off += w; a->written += w;
    }
}
