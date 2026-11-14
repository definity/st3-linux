#ifndef ST3_AUDIO_H
#define ST3_AUDIO_H
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    int fd; pid_t pid; int on;
    double phase; float pl, pr; float volume;
    int16_t obuf[8192]; int on_n;
    long dropped;
} Audio;
int audio_open(Audio *a, const char *cmd);
void audio_close(Audio *a);
void audio_feed(Audio *a, const uint8_t *d, int n, double rate, int stereo);
#endif
