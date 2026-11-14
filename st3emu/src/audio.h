#ifndef ST3_AUDIO_H
#define ST3_AUDIO_H
#include <stdint.h>
#include <sys/types.h>
typedef struct {
    int fd; pid_t pid; int on;
    double phase; float pl, pr; float volume;
    int16_t obuf[8192]; int on_n;
    long dropped, written, in_bytes; double in_rate; int in_stereo;
    uint8_t part[2]; int part_n, in_stereo_prev;   /* bytes of an incomplete frame carried to the next feed */
    double last_feed;   /* monotonic seconds of last audio_feed */
} Audio;
int audio_open(Audio *a, const char *cmd);
void audio_close(Audio *a);
double audio_queued_ms(Audio *a);   /* audio waiting in the pipe to the sink */
int audio_active(Audio *a);          /* sound was produced in the last 0.2 s */
void audio_feed(Audio *a, const uint8_t *d, int n, double rate, int stereo);
#endif
