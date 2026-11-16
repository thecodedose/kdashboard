#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* A paced virtual PCM, not ALSA's unclocked null device. Every FIFO write is
 * nonblocking and <= PIPE_BUF, so a stalled Kindle mixer cannot lock Spotify. */
typedef struct {
    snd_pcm_ioplug_t io;
    int output, timer;
    char *path;
    unsigned char *ring;
    uint64_t written, played, origin, epoch;
    size_t capacity;
    int running, paused;
} KindlePCM;

static uint64_t ns_now(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static void pump(KindlePCM *p) {
    if (!p->running || p->paused || !p->capacity || p->written == p->played) return;
    uint64_t now = ns_now();
    uint64_t due = p->origin + (now - p->epoch) * 44100 / 1000000000ULL;
    if (due <= p->played) return;
    /* Backpressure pauses our clock instead of causing a catch-up burst. */
    if (due - p->played > 4410) {
        p->origin = p->played; p->epoch = now; due = p->played + 441;
    }
    uint64_t frames = due - p->played;
    if (frames > p->written - p->played) frames = p->written - p->played;
    if (frames > 1024) frames = 1024;
    unsigned char bytes[4096];
    for (uint64_t i = 0; i < frames; ++i)
        memcpy(bytes + i * 4, p->ring + ((p->played + i) % p->capacity) * 4, 4);
    if (p->output < 0) p->output = open(p->path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (p->output < 0) return;
    ssize_t count = write(p->output, bytes, (size_t)frames * 4);
    if (count > 0) p->played += (uint64_t)count / 4;
    else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close(p->output); p->output = -1;
    }
}

static int start(snd_pcm_ioplug_t *io) {
    KindlePCM *p = io->private_data;
    p->running = 1; p->paused = 0; p->origin = p->played; p->epoch = ns_now(); return 0;
}
static int stop(snd_pcm_ioplug_t *io) {
    KindlePCM *p = io->private_data; p->running = 0; return 0;
}
static int prepare(snd_pcm_ioplug_t *io) {
    KindlePCM *p = io->private_data;
    if (!io->buffer_size || io->buffer_size > 44100) return -EINVAL;
    free(p->ring); p->ring = calloc((size_t)io->buffer_size, 4);
    if (!p->ring) return -ENOMEM;
    p->capacity = (size_t)io->buffer_size;
    p->written = p->played = p->origin = 0; p->running = p->paused = 0;
    struct itimerspec period = { .it_interval = {0, 10000000}, .it_value = {0, 10000000} };
    return timerfd_settime(p->timer, 0, &period, NULL) ? -errno : 0;
}
static snd_pcm_sframes_t pointer(snd_pcm_ioplug_t *io) {
    KindlePCM *p = io->private_data; pump(p);
    return p->capacity ? (snd_pcm_sframes_t)(p->played % p->capacity) : 0;
}
static snd_pcm_sframes_t transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas,
                                  snd_pcm_uframes_t offset, snd_pcm_uframes_t size) {
    KindlePCM *p = io->private_data; pump(p);
    size_t available = p->capacity - (size_t)(p->written - p->played);
    if (size > available) size = (snd_pcm_uframes_t)available;
    if (!size) return -EAGAIN;
    for (snd_pcm_uframes_t frame = 0; frame < size; ++frame) {
        unsigned char *dest = p->ring + ((p->written + frame) % p->capacity) * 4;
        for (unsigned ch = 0; ch < 2; ++ch) {
            const unsigned char *src = (const unsigned char *)areas[ch].addr +
                areas[ch].first / 8 + (offset + frame) * areas[ch].step / 8;
            memcpy(dest + ch * 2, src, 2);
        }
    }
    p->written += size; return (snd_pcm_sframes_t)size;
}
static int delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *result) {
    KindlePCM *p = io->private_data; pump(p);
    *result = (snd_pcm_sframes_t)(p->written - p->played); return 0;
}
static int pause_pcm(snd_pcm_ioplug_t *io, int enabled) {
    KindlePCM *p = io->private_data; pump(p); p->paused = enabled;
    if (!enabled) { p->origin = p->played; p->epoch = ns_now(); }
    return 0;
}
static int drain(snd_pcm_ioplug_t *io) {
    KindlePCM *p = io->private_data;
    if (!p->running) start(io);
    uint64_t deadline = ns_now() + 2000000000ULL;
    while (p->played < p->written) {
        pump(p);
        if (ns_now() > deadline) return -EIO;
        struct timespec step = {0, 2000000}; nanosleep(&step, NULL);
    }
    return 0;
}
static int poll_revents(snd_pcm_ioplug_t *io, struct pollfd *fds, unsigned nfds, unsigned short *events) {
    KindlePCM *p = io->private_data;
    *events = 0;
    if (nfds != 1) return -EINVAL;
    if (fds[0].revents & POLLIN) {
        uint64_t ticks;
        (void)read(p->timer, &ticks, sizeof(ticks)); pump(p);
        if (p->written - p->played < p->capacity) *events = POLLOUT;
    }
    return 0;
}
static int close_pcm(snd_pcm_ioplug_t *io) {
    KindlePCM *p = io->private_data;
    if (p->output >= 0) close(p->output);
    close(p->timer); free(p->ring); free(p->path); free(p); return 0;
}
static const snd_pcm_ioplug_callback_t callbacks = {
    .start = start, .stop = stop, .pointer = pointer, .transfer = transfer,
    .close = close_pcm, .prepare = prepare, .drain = drain,
    .pause = pause_pcm, .poll_revents = poll_revents, .delay = delay
};

SND_PCM_PLUGIN_DEFINE_FUNC(kindle_fifo) {
    (void)root;
    if (stream != SND_PCM_STREAM_PLAYBACK) return -EINVAL;
    const char *path = NULL;
    snd_config_iterator_t current, next;
    snd_config_for_each(current, next, conf) {
        snd_config_t *entry = snd_config_iterator_entry(current); const char *id;
        if (snd_config_get_id(entry, &id) < 0) continue;
        if (!strcmp(id, "type") || !strcmp(id, "comment") || !strcmp(id, "hint")) continue;
        if (strcmp(id, "path") || snd_config_get_string(entry, &path) < 0) return -EINVAL;
    }
    if (!path || path[0] != '/') return -EINVAL;
    KindlePCM *p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;
    p->output = -1; p->path = strdup(path);
    p->timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (!p->path || p->timer < 0) {
        if (p->timer >= 0) close(p->timer);
        free(p->path); free(p); return -ENOMEM;
    }
    p->io.version = SND_PCM_IOPLUG_VERSION;
    p->io.name = "Kindle paced Bluetooth bridge";
    p->io.flags = SND_PCM_IOPLUG_FLAG_MONOTONIC;
    p->io.mmap_rw = 1;
    p->io.poll_fd = p->timer; p->io.poll_events = POLLIN;
    p->io.callback = &callbacks; p->io.private_data = p;
    int rc = snd_pcm_ioplug_create(&p->io, name, stream, mode);
    if (rc < 0) { close(p->timer); free(p->path); free(p); return rc; }
    const unsigned access[] = {SND_PCM_ACCESS_RW_INTERLEAVED, SND_PCM_ACCESS_MMAP_INTERLEAVED};
    const unsigned format[] = {SND_PCM_FORMAT_S16_LE};
    snd_pcm_ioplug_set_param_list(&p->io, SND_PCM_IOPLUG_HW_ACCESS, 2, access);
    snd_pcm_ioplug_set_param_list(&p->io, SND_PCM_IOPLUG_HW_FORMAT, 1, format);
    snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2);
    snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_RATE, 44100, 44100);
    snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 176, 16384);
    snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 4096, 176400);
    snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64);
    *pcmp = p->io.pcm; return 0;
}
SND_PCM_PLUGIN_SYMBOL(kindle_fifo);
