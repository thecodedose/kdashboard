#define _GNU_SOURCE
#include "shim.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * What `ps` and logsubmit show. Must be shorter than a real Soloist key.
 * Soloist only accepts --api-key on argv. The constructor runs before
 * main(), so this must not scrub in the constructor itself. First pa_*
 * is too late: an idle daemon still shows the secret in logsubmit.
 * Sleep after load, then overwrite the slot. Do not log the real value.
 */
#define SHIM_API_KEY_DECOY "nice-try-logsubmit"
#define SHIM_API_KEY_SCRUB_DELAY_US 2000000

static int
read_arg_range(unsigned long *start, unsigned long *end)
{
    FILE *f;
    char buf[4096];
    char *rparen;
    char *tok;
    char *save;
    unsigned long field[50];
    int n = 0;

    f = fopen("/proc/self/stat", "r");
    if (!f)
        return -1;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    rparen = strrchr(buf, ')');
    if (!rparen || rparen[1] != ' ')
        return -1;
    tok = strtok_r(rparen + 2, " ", &save);
    while (tok && n < 50) {
        field[n++] = strtoul(tok, NULL, 10);
        tok = strtok_r(NULL, " ", &save);
    }
    /* field[0] is comm's neighbour (stat field 3). arg_start is 48. */
    if (n < 47)
        return -1;
    *start = field[45];
    *end = field[46];
    return 0;
}

void
shim_scrub_cmdline_secrets(void)
{
    static int done;
    unsigned long arg_start = 0, arg_end = 0;
    char *p;
    char *end;
    char *next;
    size_t decoy_len;

    if (done)
        return;
    done = 1;
    unsetenv("API_KEY");

    if (read_arg_range(&arg_start, &arg_end) != 0 || arg_end <= arg_start)
        return;
    p = (char *)(uintptr_t)arg_start;
    end = (char *)(uintptr_t)arg_end;
    decoy_len = strlen(SHIM_API_KEY_DECOY);

    while (p < end) {
        next = p + strlen(p) + 1;
        if (strcmp(p, "--api-key") == 0 || strcmp(p, "-k") == 0) {
            if (next < end && *next) {
                size_t n = strlen(next);

                memset(next, 0, n);
                memcpy(next, SHIM_API_KEY_DECOY,
                       decoy_len < n ? decoy_len : n);
                shim_log("cmdline --api-key scrubbed (%zu bytes)\n", n);
            }
            return;
        }
        p = next;
    }
}

static void *
scrub_cmdline_later(void *unused)
{
    (void)unused;
    usleep(SHIM_API_KEY_SCRUB_DELAY_US);
    shim_scrub_cmdline_secrets();
    return NULL;
}

__attribute__((constructor))
static void
shim_start_cmdline_scrub(void)
{
    pthread_t t;
    pthread_attr_t attr;

    if (pthread_attr_init(&attr) != 0)
        return;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, scrub_cmdline_later, NULL) != 0) {
        pthread_attr_destroy(&attr);
        return;
    }
    pthread_attr_destroy(&attr);
}

void
shim_log(const char *fmt, ...)
{
    static int once, on;
    va_list ap;

    if (!once) {
        const char *e = getenv("APULSE_DIAG");

        on = e && e[0] && e[0] != '0';
        once = 1;
    }
    if (!on)
        return;
    fputs("soloist-shim: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void
shim_api(const char *fn)
{
    shim_scrub_cmdline_secrets();
    if (fn)
        shim_log("%s\n", fn);
}

void
shim_api_hot(const char *fn)
{
    static const char *seen[32];
    static int count[32];
    static int n;
    int i;

    shim_scrub_cmdline_secrets();
    if (!fn)
        return;
    for (i = 0; i < n; i++) {
        if (seen[i] == fn) {
            if (count[i] >= 8) {
                if (count[i] == 8) {
                    count[i]++;
                    shim_log("%s (further calls not logged)\n", fn);
                }
                return;
            }
            count[i]++;
            shim_log("%s\n", fn);
            return;
        }
    }
    if (n < 32) {
        seen[n] = fn;
        count[n] = 1;
        n++;
    }
    shim_log("%s\n", fn);
}

const char *
shim_playback_device(void)
{
    const char *d = getenv("APULSE_PLAYBACK_DEVICE");

    return (d && d[0]) ? d : "plug:volumio";
}

/* The PCM is released when the yield file appears. With no path set there
 * is no yield: the PCM stays open until the stream disconnects. */
int
shim_yield_requested(void)
{
    const char *p = getenv("APULSE_YIELD_PATH");

    if (!p || !p[0])
        return 0;
    return access(p, F_OK) == 0;
}

int
shim_external_volume(void)
{
    const char *e = getenv("APULSE_EXTERNAL_VOLUME");

    return (e && e[0] && e[0] != '0');
}

float
shim_trim_gain(void)
{
    const char *e = getenv("APULSE_OUTPUT_TRIM_DB");
    char *end = NULL;
    double db;

    if (!e || !e[0])
        return 1.f;
    db = strtod(e, &end);
    if (end == e)
        return 1.f;
    if (db > 12)
        db = 12;
    if (db < -12)
        db = -12;
    return (float)pow(10.0, db / 20.0);
}

size_t
shim_alsa_frame_size(snd_pcm_format_t fmt, unsigned channels)
{
    int bits;

    if (!channels)
        return 0;
    bits = snd_pcm_format_physical_width(fmt);
    if (bits <= 0)
        return 0;
    return ((size_t)bits / 8) * channels;
}

const char *
shim_alsa_format_name(snd_pcm_format_t fmt)
{
    const char *n = snd_pcm_format_name(fmt);

    return n ? n : "?";
}

static void
float_to_s16le(void *buf, size_t samples)
{
    const float *in = buf;
    int16_t *out = buf;
    size_t i;

    for (i = 0; i < samples; i++) {
        float x = in[i];

        if (x > 1.f)
            x = 1.f;
        if (x < -1.f)
            x = -1.f;
        out[i] = (int16_t)lrintf(x * 32767.f);
    }
}

static void
float_to_s32le(void *buf, size_t samples)
{
    const float *in = buf;
    int32_t *out = buf;
    size_t i;

    for (i = 0; i < samples; i++) {
        float x = in[i];

        if (x > 1.f)
            x = 1.f;
        if (x < -1.f)
            x = -1.f;
        out[i] = (int32_t)lrintf(x * 2147483647.f);
    }
}

void
shim_convert_to_alsa(void *buf, size_t frames, unsigned channels,
                     pa_sample_format_t from, snd_pcm_format_t to)
{
    size_t n;

    if (!buf || !frames || !channels)
        return;
    if (from != PA_SAMPLE_FLOAT32LE)
        return;
    n = frames * channels;
    if (to == SND_PCM_FORMAT_S16_LE)
        float_to_s16le(buf, n);
    else if (to == SND_PCM_FORMAT_S32_LE)
        float_to_s32le(buf, n);
}

size_t
shim_frame_size(const pa_sample_spec *ss)
{
    size_t sample;

    if (!ss || !ss->channels)
        return 0;
    switch (ss->format) {
    case PA_SAMPLE_U8:
        sample = 1;
        break;
    case PA_SAMPLE_S16LE:
    case PA_SAMPLE_S16BE:
        sample = 2;
        break;
    case PA_SAMPLE_S24LE:
    case PA_SAMPLE_S24BE:
        sample = 3;
        break;
    case PA_SAMPLE_S24_32LE:
    case PA_SAMPLE_S24_32BE:
    case PA_SAMPLE_S32LE:
    case PA_SAMPLE_S32BE:
    case PA_SAMPLE_FLOAT32LE:
    case PA_SAMPLE_FLOAT32BE:
        sample = 4;
        break;
    default:
        return 0;
    }
    return sample * ss->channels;
}

void
shim_apply_volume(void *buf, size_t bytes, const pa_volume_t *vol,
                  const pa_sample_spec *ss)
{
    size_t fs, frames, i, ch;
    float g[PA_CHANNELS_MAX];

    if (shim_external_volume() || !buf || !vol || !ss)
        return;
    fs = shim_frame_size(ss);
    if (fs == 0)
        return;
    frames = bytes / fs;
    for (ch = 0; ch < ss->channels && ch < PA_CHANNELS_MAX; ch++)
        g[ch] = (float)vol[ch] / (float)PA_VOLUME_NORM;
    if (ss->format == PA_SAMPLE_FLOAT32LE) {
        float *p = buf;

        for (i = 0; i < frames; i++)
            for (ch = 0; ch < ss->channels; ch++)
                p[i * ss->channels + ch] *= g[ch];
    }
}

void
shim_apply_trim(void *buf, size_t bytes, const pa_sample_spec *ss)
{
    float g = shim_trim_gain();
    size_t fs, n, i;
    float *p;

    if (g == 1.f || !buf || !ss || ss->format != PA_SAMPLE_FLOAT32LE)
        return;
    fs = shim_frame_size(ss);
    if (fs == 0)
        return;
    n = (bytes / fs) * ss->channels;
    p = buf;
    for (i = 0; i < n; i++)
        p[i] *= g;
}

SHIM_EXPORT
size_t
pa_frame_size(const pa_sample_spec *spec)
{
    shim_api_hot("pa_frame_size");
    return shim_frame_size(spec);
}

SHIM_EXPORT
pa_usec_t
pa_bytes_to_usec(uint64_t length, const pa_sample_spec *spec)
{
    size_t fs = shim_frame_size(spec);

    shim_api_hot("pa_bytes_to_usec");
    if (!spec || !spec->rate || fs == 0)
        return 0;
    return (pa_usec_t)(length * 1000000ULL / (fs * (uint64_t)spec->rate));
}

SHIM_EXPORT
size_t
pa_usec_to_bytes(pa_usec_t t, const pa_sample_spec *spec)
{
    size_t fs = shim_frame_size(spec);

    shim_api_hot("pa_usec_to_bytes");
    if (!spec || !spec->rate || fs == 0)
        return 0;
    return (size_t)((uint64_t)t * spec->rate / 1000000ULL) * fs;
}

SHIM_EXPORT
struct timeval *
pa_gettimeofday(struct timeval *tv)
{
    shim_api_hot("pa_gettimeofday");
    gettimeofday(tv, NULL);
    return tv;
}

SHIM_EXPORT
pa_usec_t
pa_timeval_diff(const struct timeval *a, const struct timeval *b)
{
    const struct timeval *hi = a, *lo = b;

    shim_api_hot("pa_timeval_diff");
    if (a->tv_sec < b->tv_sec ||
        (a->tv_sec == b->tv_sec && a->tv_usec < b->tv_usec)) {
        hi = b;
        lo = a;
    }
    return (pa_usec_t)(hi->tv_sec - lo->tv_sec) * 1000000ULL +
           (pa_usec_t)(hi->tv_usec - lo->tv_usec);
}

SHIM_EXPORT
pa_cvolume *
pa_cvolume_set(pa_cvolume *a, unsigned channels, pa_volume_t v)
{
    unsigned i;

    shim_api_hot("pa_cvolume_set");
    if (!a)
        return NULL;
    a->channels = (uint8_t)channels;
    for (i = 0; i < channels && i < PA_CHANNELS_MAX; i++)
        a->values[i] = v;
    return a;
}

SHIM_EXPORT
pa_volume_t
pa_cvolume_avg(const pa_cvolume *a)
{
    unsigned i;
    uint64_t s = 0;

    shim_api_hot("pa_cvolume_avg");
    if (!a || !a->channels)
        return PA_VOLUME_MUTED;
    for (i = 0; i < a->channels; i++)
        s += a->values[i];
    return (pa_volume_t)(s / a->channels);
}

SHIM_EXPORT
const char *
pa_strerror(int error)
{
    (void)error;
    shim_api_hot("pa_strerror");
    return "error";
}

SHIM_EXPORT
pa_proplist *
pa_proplist_new(void)
{
    shim_api("pa_proplist_new");
    return calloc(1, sizeof(pa_proplist));
}

SHIM_EXPORT
void
pa_proplist_free(pa_proplist *p)
{
    size_t i;

    if (!p)
        return;
    shim_api("pa_proplist_free");
    for (i = 0; i < p->n; i++) {
        free(p->k[i]);
        free(p->v[i]);
    }
    free(p->k);
    free(p->v);
    free(p);
}

SHIM_EXPORT
int
pa_proplist_sets(pa_proplist *p, const char *key, const char *value)
{
    shim_api("pa_proplist_sets");
    if (!p || !key)
        return -1;
    p->k = realloc(p->k, (p->n + 1) * sizeof(char *));
    p->v = realloc(p->v, (p->n + 1) * sizeof(char *));
    p->k[p->n] = strdup(key);
    p->v[p->n] = value ? strdup(value) : strdup("");
    p->n++;
    return 0;
}

SHIM_EXPORT
pa_operation_state_t
pa_operation_get_state(pa_operation *o)
{
    shim_api_hot("pa_operation_get_state");
    return o ? o->state : PA_OPERATION_DONE;
}

SHIM_EXPORT
void
pa_operation_unref(pa_operation *o)
{
    if (!o)
        return;
    shim_api_hot("pa_operation_unref");
    if (--o->ref > 0)
        return;
    free(o);
}

pa_operation *
shim_op_new(pa_mainloop_api *api, void (*run)(pa_operation *))
{
    pa_operation *op = calloc(1, sizeof(*op));

    op->ref = 1;
    op->state = PA_OPERATION_RUNNING;
    op->api = api;
    op->run = run;
    return op;
}

void
shim_op_done(pa_operation *op)
{
    op->state = PA_OPERATION_DONE;
    pa_operation_unref(op);
}
