#include <alsa/asoundlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Exercise format conversion, clock pacing and stalled FIFO backpressure. */
int main(int argc, char **argv) {
    int stalled = argc == 2 && !strcmp(argv[1], "--stalled");
    alarm(5);
    snd_pcm_t *pcm;
    int rc = snd_pcm_open(&pcm, "kindle", SND_PCM_STREAM_PLAYBACK, stalled ? SND_PCM_NONBLOCK : 0);
    if (rc < 0) { fprintf(stderr, "open: %s\n", snd_strerror(rc)); return 1; }
    rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1, 200000);
    if (rc < 0) { fprintf(stderr, "params: %s\n", snd_strerror(rc)); return 1; }
    float samples[4410 * 2];
    for (unsigned i = 0; i < sizeof(samples) / sizeof(*samples); ++i) samples[i] = 0.25f;
    if (stalled) {
        unsigned blocked = 0;
        for (unsigned i = 0; i < 100; ++i) {
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, samples, 4410);
            if (n == -EAGAIN) ++blocked;
            else if (n < 0) { fprintf(stderr, "stalled write: %s\n", snd_strerror((int)n)); return 1; }
            usleep(10000);
        }
        snd_pcm_sframes_t queued = 0;
        rc = snd_pcm_delay(pcm, &queued);
        if (rc || !blocked || queued <= 0) { fprintf(stderr, "backpressure: rc=%d blocked=%u delay=%ld\n", rc, blocked, (long)queued); return 1; }
        snd_pcm_drop(pcm); snd_pcm_close(pcm);
        printf("Stalled FIFO remained responsive; queued=%ld frames\n", (long)queued);
        return 0;
    }
    struct timespec begin, end; clock_gettime(CLOCK_MONOTONIC, &begin);
    snd_pcm_sframes_t frames = snd_pcm_writei(pcm, samples, 4410);
    if (frames != 4410) { fprintf(stderr, "write: %ld\n", (long)frames); return 1; }
    rc = snd_pcm_drain(pcm); snd_pcm_close(pcm);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) / 1e9;
    if (rc || elapsed < 0.08 || elapsed > 1) { fprintf(stderr, "drain: %d elapsed %.3f\n", rc, elapsed); return 1; }
    printf("Paced playback completed in %.3f seconds\n", elapsed);
    return 0;
}
