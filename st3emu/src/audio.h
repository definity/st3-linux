#ifndef ST3_AUDIO_H
#define ST3_AUDIO_H
#include <stdint.h>
#include <sys/types.h>
/* audio.c — streams the emulator's mixed output (48 kHz s16 stereo) to a sink command (default: pw-play). */
typedef struct {
    int fd; pid_t pid;
    long dropped, written;   /* bytes */
    double last_write;       /* monotonic seconds */
} Audio;
int audio_open(Audio *a, const char *cmd);
void audio_close(Audio *a);
void audio_write(Audio *a, const int16_t *frames, int nframes);
double audio_queued_ms(Audio *a);   /* audio waiting in the pipe to the sink */
int audio_active(Audio *a);          /* audio was written in the last 0.2 s */
#endif
