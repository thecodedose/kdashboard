#include "shim.h"

#include <errno.h>
#include <glob.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Soloist writes 8–32 KiB even when writable_size is smaller. The ring
 * must be larger than tlength or pa_stream_write drops the tail and
 * the track plays in shreds (fast + choppy). Default buffer_ms=500 is
 * exactly 0.5 s of FLOAT32, which was the old ring size. */
#define SHIM_WRITE_SLACK 65536

static volatile int pcm_closes_in_flight;

static void stream_release(pa_stream *s, int keep_position);
static void stream_clock_reset(pa_stream *s);
static void stream_clock_freeze(pa_stream *s);
static void stream_clock_hold(pa_stream *s);
static void stream_clock_start(pa_stream *s);
static int64_t stream_played_bytes(pa_stream *s);
static void stream_set_state(pa_stream *s, pa_stream_state_t st);
static int stream_open_pcm(pa_stream *s);
static void stream_schedule_reopen(pa_stream *s);
static void stream_arm_underflow(pa_stream *s);
static void stream_cancel_underflow(pa_stream *s);

static void
adjust_attr(pa_stream *s, const pa_buffer_attr *in)
{
    size_t fs = shim_frame_size(&s->ss);
    pa_buffer_attr *a = &s->attr;
    const char *cap;
    long ms;

    if (in)
        *a = *in;
    else
        memset(a, 0xff, sizeof(*a));
    if (a->maxlength == (uint32_t)-1)
        a->maxlength = 4 * 1024 * 1024;
    if (a->tlength == (uint32_t)-1)
        a->tlength = (uint32_t)pa_usec_to_bytes(2 * 1000 * 1000, &s->ss);
    cap = getenv("APULSE_MAX_TLENGTH_MS");
    ms = cap ? strtol(cap, NULL, 10) : 0;
    if (ms > 0 && fs && s->ss.rate) {
        uint32_t c = (uint32_t)pa_usec_to_bytes((pa_usec_t)ms * 1000, &s->ss);

        if (c >= fs && a->tlength > c)
            a->tlength = c;
    }
    if (a->minreq == (uint32_t)-1)
        a->minreq = (uint32_t)pa_usec_to_bytes(20 * 1000, &s->ss);
    if (a->minreq > a->tlength / 4 && a->tlength / 4 >= fs)
        a->minreq = a->tlength / 4;
    if (a->prebuf == (uint32_t)-1)
        a->prebuf = a->tlength - a->minreq;
    {
        size_t need = (size_t)a->tlength + SHIM_WRITE_SLACK;

        if (need < 72 * 1024)
            need = 72 * 1024;
        if (!s->rb) {
            s->rb = ring_new(need);
        } else if (ring_capacity(s->rb) < need && ring_readable(s->rb) == 0) {
            ring_free(s->rb);
            s->rb = ring_new(need);
        }
    }
}

static snd_pcm_format_t
client_format(pa_sample_format_t f)
{
    switch (f) {
    case PA_SAMPLE_S16LE:
        return SND_PCM_FORMAT_S16_LE;
    case PA_SAMPLE_S16BE:
        return SND_PCM_FORMAT_S16_BE;
    case PA_SAMPLE_FLOAT32LE:
        return SND_PCM_FORMAT_FLOAT_LE;
    case PA_SAMPLE_FLOAT32BE:
        return SND_PCM_FORMAT_FLOAT_BE;
    case PA_SAMPLE_S32LE:
        return SND_PCM_FORMAT_S32_LE;
    case PA_SAMPLE_S24_32LE:
        return SND_PCM_FORMAT_S24_LE;
    default:
        return SND_PCM_FORMAT_FLOAT_LE;
    }
}

/*
 * Application format is what we writei. That is the client spec, always.
 *
 * plug:volumio is a converter (or a converter in front of volumioswitch).
 * Setting S16/S32 here does not "help" a device without softvolume: on an
 * ioplug with format_append it becomes the format the USB/AML slave opens,
 * which is how 0.2.1 packed a 32-bit slot device as S16 and rushed.
 * Packed S24_3LE is never the client format, so it is never opened.
 */
static int
set_app_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
               pa_sample_format_t client, snd_pcm_format_t *out)
{
    snd_pcm_format_t fmt = client_format(client);

    if (snd_pcm_hw_params_set_format(pcm, hw, fmt) < 0)
        return -1;
    *out = fmt;
    return 0;
}

static int
pick_period(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, snd_pcm_uframes_t want,
            snd_pcm_uframes_t *out)
{
    snd_pcm_hw_params_t *tmp;
    snd_pcm_uframes_t p = want;
    int dir = 0;
    int err;

    if (snd_pcm_hw_params_malloc(&tmp) < 0)
        return -1;
    snd_pcm_hw_params_copy(tmp, hw);
    if (p > 0)
        err = snd_pcm_hw_params_set_period_size_near(pcm, tmp, &p, &dir);
    else
        err = snd_pcm_hw_params_set_period_size_first(pcm, tmp, &p, &dir);
    if (err < 0 || p == 0) {
        snd_pcm_hw_params_copy(tmp, hw);
        p = 0;
        dir = 0;
        err = snd_pcm_hw_params_set_period_size_first(pcm, tmp, &p, &dir);
    }
    snd_pcm_hw_params_free(tmp);
    if (err < 0 || p == 0)
        return -1;
    *out = p;
    return 0;
}

static int
pick_buffer(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, snd_pcm_uframes_t period,
            snd_pcm_uframes_t want, snd_pcm_uframes_t *out)
{
    snd_pcm_uframes_t n, k;

    if (period == 0)
        return -1;
    n = want / period;
    if (n < 4)
        n = 4;
    if (snd_pcm_hw_params_test_buffer_size(pcm, hw, n * period) == 0) {
        *out = n * period;
        return 0;
    }
    if (snd_pcm_hw_params_test_buffer_size(pcm, hw, 4 * period) == 0) {
        *out = 4 * period;
        return 0;
    }
    for (k = 16; k >= 2; k--) {
        if (snd_pcm_hw_params_test_buffer_size(pcm, hw, k * period) == 0) {
            *out = k * period;
            return 0;
        }
    }
    return -1;
}

void
shim_stream_set_output(pa_stream *s, int enable)
{
    pa_io_event_flags_t ev;
    int i;

    if (!s || !s->ioe || s->out_enabled == enable)
        return;
    ev = enable ? s->ioe_events : (s->ioe_events & ~PA_IO_EVENT_OUTPUT);
    for (i = 0; i < s->nioe; i++)
        if (s->ioe[i])
            s->c->api->io_enable(s->ioe[i], ev);
    s->out_enabled = enable;
}

static int
stream_prepare(pa_stream *s)
{
    snd_pcm_sframes_t avail;

    if (!s->pcm)
        return -1;
    if (s->clock_running && !s->paused &&
        snd_pcm_state(s->pcm) == SND_PCM_STATE_XRUN)
        stream_played_bytes(s);
    stream_clock_freeze(s);
    if (snd_pcm_prepare(s->pcm) < 0) {
        shim_log("pcm prepare failed\n");
        shim_stream_set_output(s, 0);
        return -1;
    }
    avail = snd_pcm_avail(s->pcm);
    if (avail < 0) {
        /* prepare on the switcher leaves volumioOutput dead (Motivo).
         * This is the one underflow of the spell: the latch is taken so a
         * dry-ring check already armed does not report a second. */
        s->drop_unsafe = 1;
        shim_stream_set_output(s, 0);
        stream_cancel_underflow(s);
        s->underflow_sent = 1;
        if (s->underflow_cb)
            s->underflow_cb(s, s->underflow_cb_userdata);
        return -1;
    }
    return 0;
}

static void
io_cb(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t events,
      void *userdata)
{
    pa_stream *s = userdata;
    snd_pcm_sframes_t avail, wr;
    size_t fs, got, nbytes;
    int paused;

    (void)a;
    (void)e;
    (void)fd;
    shim_stream_maybe_yield(s);
    if (!s->pcm)
        return;
    if (!(events & (PA_IO_EVENT_INPUT | PA_IO_EVENT_OUTPUT)))
        return;

    fs = shim_frame_size(&s->ss);
    if (fs == 0 || !s->io_buf || !s->alsa_fs)
        return;
    avail = snd_pcm_avail(s->pcm);
    if (avail < 0) {
        if (avail == -EBADFD || avail == -EAGAIN)
            return;
        shim_log("avail %s\n", snd_strerror((int)avail));
        if (stream_prepare(s) < 0)
            stream_schedule_reopen(s);
        return;
    }
    if (avail <= 0) {
        if (s->paused || ring_readable(s->rb) < fs)
            return;
        if (snd_pcm_state(s->pcm) != SND_PCM_STATE_PREPARED)
            return;
        avail = (snd_pcm_sframes_t)(s->io_buf_bytes / fs);
        if (avail <= 0)
            return;
    }
    /*
     * volumioswitch avail is local + target and can be a second or more.
     * Writing that in one wakeup is a USB/AML fill, not the DAC clock.
     * Cap to two periods. That is ALSA I/O, not pace matching.
     */
    if (snd_pcm_state(s->pcm) == SND_PCM_STATE_RUNNING && s->period &&
        avail > (snd_pcm_sframes_t)(s->period * 2))
        avail = (snd_pcm_sframes_t)(s->period * 2);
    nbytes = (size_t)avail * fs;
    if (nbytes > s->io_buf_bytes)
        nbytes = s->io_buf_bytes;
    avail = (snd_pcm_sframes_t)(nbytes / fs);
    if (avail <= 0)
        return;

    paused = s->paused;
    if (!(events & PA_IO_EVENT_OUTPUT))
        return;

    if (paused) {
        if (snd_pcm_state(s->pcm) != SND_PCM_STATE_RUNNING)
            return;
        memset(s->io_buf, 0, (size_t)avail * s->alsa_fs);
        wr = snd_pcm_writei(s->pcm, s->io_buf, (snd_pcm_uframes_t)avail);
        if (wr < 0 && wr != -EAGAIN && stream_prepare(s) < 0)
            stream_schedule_reopen(s);
        return;
    }

    got = ring_peek(s->rb, s->io_buf, (size_t)avail * fs);
    if (got < fs) {
        shim_stream_set_output(s, 0);
        stream_arm_underflow(s);
        return;
    }
    got = (got / fs) * fs;
    shim_apply_volume(s->io_buf, got, s->volume, &s->ss);
    shim_apply_trim(s->io_buf, got, &s->ss);
    shim_convert_to_alsa(s->io_buf, got / fs, s->ss.channels, s->ss.format,
                         s->alsa_fmt);
    wr = snd_pcm_writei(s->pcm, s->io_buf, got / fs);
    if (wr < 0 && wr != -EAGAIN) {
        shim_log("writei %s, avail=%ld\n", snd_strerror((int)wr), (long)avail);
        if (stream_prepare(s) < 0)
            stream_schedule_reopen(s);
        return;
    }
    if (wr > 0) {
        ring_drop(s->rb, (size_t)wr * fs);
        if (ring_readable(s->rb) < fs) {
            shim_stream_set_output(s, 0);
            stream_arm_underflow(s);
        }
        stream_clock_start(s);
        if (s->started_cb)
            s->started_cb(s, s->started_cb_userdata);
        s->started_cb = NULL;
    }
}

/*
 * The poll reported the PCM in error: an underrun on a hardware device,
 * a hangup, a dead descriptor. The mainloop hands it here before the I/O
 * callback runs, so the recovery is the one stream_prepare does, with the
 * position sampled, the clock frozen and the line in the diag, instead of
 * a bare prepare nobody sees. The I/O callback follows with the PREPARED
 * device: it writes if the ring has anything, otherwise it parks the
 * output and arms the underflow.
 */
void
shim_stream_poll_error(pa_stream *s)
{
    if (!s || !s->pcm)
        return;
    shim_log("poll error, pcm %s\n",
             snd_pcm_state_name(snd_pcm_state(s->pcm)));
    if (stream_prepare(s) < 0)
        stream_schedule_reopen(s);
}

/*
 * Underflow, as a Pulse server reports it: once per dry spell, after
 * ALSA has played out what it held when the ring ran dry.
 *
 * Each wait is what the PCM says it still holds, plus a margin; zero is
 * the PCM saying it is empty and gets the margin alone; a PCM that will
 * not say gets the whole buffer. At the check, a PCM that has stopped,
 * or reports nothing left, is dry. A PCM that is still running with a
 * delay that has moved since the last look is playing: down as it
 * consumes, up by a frame or two when a hardware pointer estimate steps
 * back; the new figure is waited out and the look repeated. One whose
 * delay is exactly where it was is a software plugin with a stuck figure,
 * and it is dry. A cap of twice the configured buffer ends any spell. Re-armed by the next write that
 * queues data, and by flush. Never without a PCM.
 */
static unsigned
stream_delay_ms(pa_stream *s, snd_pcm_sframes_t delay)
{
    if (delay <= 0 || !s->ss.rate)
        return 0;
    return (unsigned)((uint64_t)delay * 1000 / s->ss.rate);
}

static unsigned
stream_dry_cap_ms(pa_stream *s)
{
    return (unsigned)(2 * (s->configured_sink_usec / 1000)) + SHIM_UNDERFLOW_MARGIN_MS;
}

static long
stream_dry_elapsed_ms(pa_stream *s)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (now.tv_sec - s->dry_since.tv_sec) * 1000L +
           (now.tv_usec - s->dry_since.tv_usec) / 1000L;
}

static void
underflow_check(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv,
                void *userdata)
{
    pa_stream *s = userdata;
    snd_pcm_sframes_t delay = 0;
    size_t fs;
    int dry;

    (void)tv;
    if (a && a->time_free)
        a->time_free(e);
    if (s->underflow_ev == e)
        s->underflow_ev = NULL;
    if (s->underflow_sent || s->paused || !s->pcm || !s->rb)
        return;
    fs = shim_frame_size(&s->ss);
    if (!fs || ring_readable(s->rb) >= fs)
        return;
    if (snd_pcm_state(s->pcm) != SND_PCM_STATE_RUNNING)
        dry = 1;
    else if (snd_pcm_delay(s->pcm, &delay) != 0 || delay <= 0)
        dry = 1;
    else if (stream_dry_elapsed_ms(s) >= (long)stream_dry_cap_ms(s))
        dry = 1;
    else if (s->dry_last_delay < 0 || delay != s->dry_last_delay)
        dry = 0;
    else
        dry = 1;
    if (!dry) {
        s->dry_last_delay = delay;
        s->dry_wait_ms = stream_delay_ms(s, delay) + SHIM_UNDERFLOW_MARGIN_MS;
        s->underflow_ev = shim_after_ms(s->c->api, s->dry_wait_ms, underflow_check, s);
        return;
    }
    s->underflow_sent = 1;
    shim_log("underflow after %ld ms, pcm %s delay=%ld\n", stream_dry_elapsed_ms(s),
             snd_pcm_state_name(snd_pcm_state(s->pcm)), (long)delay);
    if (s->underflow_cb)
        s->underflow_cb(s, s->underflow_cb_userdata);
}

static void
stream_arm_underflow(pa_stream *s)
{
    snd_pcm_sframes_t delay = 0;

    if (s->underflow_sent || s->underflow_ev || s->paused || !s->pcm)
        return;
    gettimeofday(&s->dry_since, NULL);
    if (snd_pcm_delay(s->pcm, &delay) == 0) {
        s->dry_last_delay = delay > 0 ? delay : 0;
        s->dry_wait_ms = stream_delay_ms(s, delay) + SHIM_UNDERFLOW_MARGIN_MS;
    } else {
        delay = -1;
        s->dry_last_delay = -1;
        s->dry_wait_ms = (unsigned)(s->configured_sink_usec / 1000) + SHIM_UNDERFLOW_MARGIN_MS;
    }
    shim_log("ring dry, pcm %s delay=%ld wait=%ums\n",
             snd_pcm_state_name(snd_pcm_state(s->pcm)), (long)delay,
             s->dry_wait_ms);
    s->underflow_ev = shim_after_ms(s->c->api, s->dry_wait_ms, underflow_check, s);
}

static void
stream_cancel_underflow(pa_stream *s)
{
    if (s->underflow_ev && s->c->api->time_free)
        s->c->api->time_free(s->underflow_ev);
    s->underflow_ev = NULL;
}

static void *
pcm_close_worker(void *arg)
{
    snd_pcm_t *pcm = arg;

    snd_pcm_nonblock(pcm, 1);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    __sync_fetch_and_sub(&pcm_closes_in_flight, 1);
    return NULL;
}

static void
stream_abandon_pcm(snd_pcm_t *pcm)
{
    pthread_t t;
    pthread_attr_t attr;

    if (!pcm)
        return;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    __sync_fetch_and_add(&pcm_closes_in_flight, 1);
    if (pthread_create(&t, &attr, pcm_close_worker, pcm) != 0) {
        __sync_fetch_and_sub(&pcm_closes_in_flight, 1);
        shim_log("pcm close worker failed, abandoning handle\n");
    }
    pthread_attr_destroy(&attr);
}

static void
quiet_alsa(const char *file, int line, const char *fn, int err, const char *fmt,
           ...)
{
    (void)file;
    (void)line;
    (void)fn;
    (void)err;
    (void)fmt;
}

static void
stream_reset_acquire(pa_stream *s)
{
    s->acquire_attempts = 0;
    s->busy_waits = 0;
    s->busy_wait_logged = 0;
    s->busy_fail_logged = 0;
}

/* Playback nodes held by someone other than this process. Used only to
 * name a waiter or a single hard fail, not to decide whether to open:
 * plug:volumio's EBUSY is the truth. */
static void
pcm_holders_note(char *buf, size_t buflen)
{
    glob_t g;
    pid_t self = getpid();
    size_t used = 0;
    size_t i;

    if (!buf || buflen < 2)
        return;
    buf[0] = '\0';
    if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &g) != 0)
        return;
    for (i = 0; i < g.gl_pathc; i++) {
        FILE *f = fopen(g.gl_pathv[i], "r");
        char line[128];
        pid_t pid = 0;

        if (!f)
            continue;
        while (fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "owner_pid");

            if (!p)
                continue;
            p = strchr(p, ':');
            if (!p)
                continue;
            pid = (pid_t)strtoul(p + 1, NULL, 10);
            break;
        }
        fclose(f);
        if (pid <= 0 || pid == self)
            continue;
        {
            char commpath[64];
            char comm[32];
            FILE *cf;
            size_t n;

            comm[0] = '?';
            comm[1] = '\0';
            snprintf(commpath, sizeof(commpath), "/proc/%ld/comm", (long)pid);
            cf = fopen(commpath, "r");
            if (cf) {
                if (fgets(comm, sizeof(comm), cf)) {
                    n = strlen(comm);
                    if (n && comm[n - 1] == '\n')
                        comm[n - 1] = '\0';
                }
                fclose(cf);
            }
            n = (size_t)snprintf(buf + used, buflen - used, "%s%s=%ld",
                                 used ? " " : "", comm, (long)pid);
            if (n >= buflen - used)
                break;
            used += n;
        }
    }
    globfree(&g);
}

static int
stream_open_pcm(pa_stream *s)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t period = 0, buffer = 0, want_p = 0, want_b = 0;
    unsigned rate;
    int err, dir = 0, nfds, k;
    size_t fs = shim_frame_size(&s->ss);
    struct pollfd *fds;
    const char *dev = shim_playback_device();

    if (s->pcm)
        return 0;
    if (shim_yield_requested())
        return -1;
    if (pcm_closes_in_flight)
        return -1;

    snd_lib_error_set_handler(quiet_alsa);
    err = snd_pcm_open(&s->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    snd_lib_error_set_handler(NULL);
    if (err == -EBUSY) {
        char holders[256];

        s->pcm = NULL;
        s->busy_waits++;
        pcm_holders_note(holders, sizeof(holders));
        if (!s->busy_wait_logged) {
            s->busy_wait_logged = 1;
            shim_log("pcm wait %s%s%s\n", dev, holders[0] ? " holder=" : "",
                     holders);
        }
        if (s->busy_waits >= 7 && !s->busy_fail_logged) {
            s->busy_fail_logged = 1;
            shim_log("pcm open %s failed: Device or resource busy%s%s\n", dev,
                     holders[0] ? " holder=" : "", holders);
        }
        return -1;
    }
    if (err < 0) {
        shim_log("pcm open %s failed: %s\n", dev, snd_strerror(err));
        return -1;
    }
    stream_reset_acquire(s);

    snd_pcm_hw_params_malloc(&hw);
    snd_pcm_hw_params_any(s->pcm, hw);
    snd_pcm_hw_params_set_access(s->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (set_app_format(s->pcm, hw, s->ss.format, &s->alsa_fmt) < 0)
        goto fail;
    s->alsa_fs = shim_alsa_frame_size(s->alsa_fmt, s->ss.channels);
    if (s->alsa_fs == 0)
        goto fail;
    /* Application rate is the client's. The outer plug resamples to the slave.
     * set_rate_near on an ioplug can snap 44.1 to 48 or 88.2 and play fast.
     * If the exact rate is refused, fail: do not write 44.1 frames at 48 k. */
    snd_pcm_hw_params_set_rate_resample(s->pcm, hw, 1);
    rate = s->ss.rate;
    if (snd_pcm_hw_params_set_rate(s->pcm, hw, rate, 0) < 0) {
        unsigned near = rate;

        dir = 0;
        if (snd_pcm_hw_params_set_rate_near(s->pcm, hw, &near, &dir) < 0)
            goto fail;
        if (near != s->ss.rate) {
            shim_log("pcm rate %u refused (near=%u)\n", s->ss.rate, near);
            goto fail;
        }
        rate = near;
    }
    if (snd_pcm_hw_params_set_channels(s->pcm, hw, s->ss.channels) < 0)
        goto fail;
    if (fs)
        want_p = s->attr.minreq / fs;
    if (pick_period(s->pcm, hw, want_p, &period) < 0)
        goto fail;
    if (snd_pcm_hw_params_set_period_size(s->pcm, hw, period, 0) < 0)
        goto fail;
    if (fs)
        want_b = s->attr.tlength / fs;
    if (pick_buffer(s->pcm, hw, period, want_b, &buffer) < 0)
        goto fail;
    if (snd_pcm_hw_params_set_buffer_size(s->pcm, hw, buffer) < 0)
        goto fail;
    if (snd_pcm_hw_params(s->pcm, hw) < 0)
        goto fail;
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    snd_pcm_hw_params_free(hw);
    hw = NULL;

    s->period = period;
    s->io_buf_bytes = (size_t)period * 4 * fs;
    if (s->io_buf_bytes < fs)
        s->io_buf_bytes = fs * 4;
    free(s->io_buf);
    s->io_buf = malloc(s->io_buf_bytes);
    if (!s->io_buf)
        goto fail;

    snd_pcm_sw_params_malloc(&sw);
    snd_pcm_sw_params_current(s->pcm, sw);
    snd_pcm_sw_params_set_avail_min(s->pcm, sw, period);
    snd_pcm_sw_params(s->pcm, sw);
    snd_pcm_sw_params_free(sw);
    if (snd_pcm_prepare(s->pcm) < 0)
        goto fail;

    nfds = snd_pcm_poll_descriptors_count(s->pcm);
    fds = calloc((size_t)nfds, sizeof(*fds));
    s->ioe = calloc((size_t)nfds, sizeof(*s->ioe));
    s->nioe = nfds;
    snd_pcm_poll_descriptors(s->pcm, fds, nfds);
    for (k = 0; k < nfds; k++) {
        pa_io_event_flags_t ev = 0;

        if (fds[k].events & POLLIN)
            ev |= PA_IO_EVENT_INPUT;
        if (fds[k].events & POLLOUT)
            ev |= PA_IO_EVENT_OUTPUT;
        s->ioe[k] = s->c->api->io_new(s->c->api, fds[k].fd, ev, io_cb, s);
        s->ioe[k]->pcm = s->pcm;
        s->ioe_events = ev;
    }
    s->out_enabled = 1;
    free(fds);
    if (s->ss.rate)
        s->configured_sink_usec =
            (pa_usec_t)((uint64_t)buffer * 1000000ULL / s->ss.rate);
    shim_log("pcm open %s period=%lu buffer=%lu rate=%u client_fmt=%d alsa=%s\n",
             dev, (unsigned long)period, (unsigned long)buffer, rate,
             (int)s->ss.format, shim_alsa_format_name(s->alsa_fmt));
    return 0;

fail:
    if (hw)
        snd_pcm_hw_params_free(hw);
    s->alsa_fs = 0;
    if (s->pcm) {
        stream_abandon_pcm(s->pcm);
        s->pcm = NULL;
    }
    return -1;
}

static void
stream_detach_io(pa_stream *s)
{
    int i;

    if (!s->ioe)
        return;
    for (i = 0; i < s->nioe; i++)
        if (s->ioe[i])
            s->c->api->io_free(s->ioe[i]);
    free(s->ioe);
    s->ioe = NULL;
    s->nioe = 0;
    s->out_enabled = 0;
}

static void
stream_close_pcm(pa_stream *s, int keep_position)
{
    stream_cancel_underflow(s);
    s->underflow_sent = 0;
    if (s->pcm) {
        stream_abandon_pcm(s->pcm);
        s->pcm = NULL;
        shim_log("pcm close handed off keep=%d\n", keep_position);
    }
    free(s->io_buf);
    s->io_buf = NULL;
    s->io_buf_bytes = 0;
    if (s->rb)
        ring_drop(s->rb, ring_readable(s->rb));
    if (keep_position)
        stream_clock_hold(s);
    else {
        s->timing.write_index = 0;
        s->timing.read_index = 0;
        stream_clock_reset(s);
    }
}

void
shim_finish_pending_releases(pa_mainloop *m)
{
    pa_stream **list;
    int i, n;

    if (!m || m->npending <= 0)
        return;
    list = m->pending_release;
    n = m->npending;
    m->pending_release = NULL;
    m->npending = 0;
    for (i = 0; i < n; i++) {
        pa_stream *s = list[i];

        stream_detach_io(s);
        stream_close_pcm(s, s->release_keep_position);
        s->release_pending = 0;
        pa_stream_unref(s);
    }
    free(list);
}

static void
stream_release(pa_stream *s, int keep_position)
{
    if (!s || (!s->pcm && !s->release_pending))
        return;
    stream_detach_io(s);
    stream_close_pcm(s, keep_position);
    s->release_pending = 0;
}

int
shim_stream_acquire(pa_stream *s)
{
    if (s->pcm)
        return 0;
    if (s->reopen_wait)
        return -1;
    if (pcm_closes_in_flight)
        return -1;
    if (shim_yield_requested())
        return -1;
    if (stream_open_pcm(s) < 0)
        return -1;
    shim_stream_set_output(s, 0);
    return 0;
}

static void
acquire_retry(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv,
              void *userdata)
{
    pa_stream *s = userdata;

    (void)tv;
    s->acquire_ev = NULL;
    if (a && a->time_free)
        a->time_free(e);
    {
        int waiting = s->reopen_wait;

        s->reopen_wait = 0;
        if (!s->want_running && !waiting)
            return;
        if (shim_stream_acquire(s) == 0) {
            if (s->want_running) {
                s->paused = 0;
                stream_clock_start(s);
                shim_stream_set_output(s, 1);
            }
            return;
        }
        if (s->want_running || waiting)
            shim_stream_schedule_acquire(s);
    }
}

void
shim_stream_schedule_acquire(pa_stream *s)
{
    static const unsigned back[] = {50, 100, 200, 400, 800};
    unsigned i, ms;

    if (s->acquire_ev && s->c->api->time_free) {
        s->c->api->time_free(s->acquire_ev);
        s->acquire_ev = NULL;
    }
    if (shim_yield_requested()) {
        s->acquire_ev = shim_after_ms(s->c->api, 50, acquire_retry, s);
        return;
    }
    i = (unsigned)s->acquire_attempts;
    if (i >= 5)
        i = 4;
    ms = back[i];
    if (s->acquire_attempts < 5)
        s->acquire_attempts++;
    s->acquire_ev = shim_after_ms(s->c->api, ms, acquire_retry, s);
}

static void
stream_reopen_pcm(pa_stream *s)
{
    stream_detach_io(s);
    if (s->pcm) {
        stream_abandon_pcm(s->pcm);
        s->pcm = NULL;
    }
    shim_stream_set_output(s, 0);
    if (shim_yield_requested())
        return;
    s->reopen_wait = 1;
    stream_reset_acquire(s);
    shim_log("pcm dead, reopen scheduled\n");
    shim_stream_schedule_acquire(s);
}

static void
reopen_defer(pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
    pa_stream *s = userdata;

    a->defer_free(e);
    s->reopen_pending = 0;
    if (s->ref > 1 && s->state == PA_STREAM_READY)
        stream_reopen_pcm(s);
    pa_stream_unref(s);
}

static void
stream_schedule_reopen(pa_stream *s)
{
    if (!s || s->reopen_pending || !s->c || !s->c->api || !s->c->api->defer_new)
        return;
    if (s->state != PA_STREAM_READY)
        return;
    s->reopen_pending = 1;
    s->ref++;
    s->c->api->defer_new(s->c->api, reopen_defer, s);
}

void
shim_stream_maybe_yield(pa_stream *s)
{
    if (!s || !s->pcm || !shim_yield_requested())
        return;
    if (s->acquire_ev && s->c->api->time_free) {
        s->c->api->time_free(s->acquire_ev);
        s->acquire_ev = NULL;
    }
    stream_reset_acquire(s);
    stream_clock_freeze(s);
    stream_release(s, 1);
}

static void
stream_clock_reset(pa_stream *s)
{
    s->clock_frozen_bytes = 0;
    s->clock_last_played_bytes = 0;
    s->clock_running = 0;
}

static void
stream_clock_start(pa_stream *s)
{
    s->clock_running = 1;
}

static void
stream_clock_freeze(pa_stream *s)
{
    if (!s->clock_running)
        return;
    s->clock_frozen_bytes = s->clock_last_played_bytes;
    s->clock_running = 0;
}

static void
stream_clock_hold(pa_stream *s)
{
    int64_t held = s->clock_frozen_bytes ? s->clock_frozen_bytes
                                          : s->clock_last_played_bytes;

    stream_clock_reset(s);
    s->clock_frozen_bytes = held;
    s->clock_last_played_bytes = held;
    s->timing.write_index = held;
    s->timing.read_index = held;
}

static int64_t
stream_played_bytes(pa_stream *s)
{
    snd_pcm_sframes_t delay = 0;
    size_t fs, queued;
    int64_t played;

    if (!s->clock_running)
        return s->clock_frozen_bytes;
    fs = shim_frame_size(&s->ss);
    if (!fs || !s->ss.rate)
        return s->clock_last_played_bytes;

    queued = ring_readable(s->rb);
    if (s->pcm && snd_pcm_delay(s->pcm, &delay) == 0 && delay > 0)
        queued += (size_t)delay * fs;

    played = s->timing.write_index - (int64_t)queued;
    if (played < 0)
        played = 0;
    if (played < s->clock_last_played_bytes)
        played = s->clock_last_played_bytes;
    s->clock_last_played_bytes = played;
    return played;
}

void
shim_stream_update_timing(pa_stream *s)
{
    int64_t played;
    size_t fs = shim_frame_size(&s->ss);

    shim_stream_maybe_yield(s);
    gettimeofday(&s->timing.timestamp, NULL);
    played = stream_played_bytes(s);
    if (fs)
        played -= played % (int64_t)fs;
    if (s->timing.write_index > 0 && played < s->timing.read_index)
        played = s->timing.read_index;
    if (played > s->timing.write_index)
        played = s->timing.write_index;
    if (played < 0)
        played = 0;
    s->timing.read_index = played;
    s->timing.read_index_corrupt = 0;
    s->timing.write_index_corrupt = 0;
    s->timing.playing = !s->paused;
    s->timing.sink_usec = 0;
    s->timing.transport_usec = 0;
    s->timing.configured_sink_usec = s->configured_sink_usec;
}

static void
stream_set_state(pa_stream *s, pa_stream_state_t st)
{
    if (s->state == st)
        return;
    s->state = st;
    if (s->state_cb)
        s->state_cb(s, s->state_cb_userdata);
}

static pa_stream *
stream_alloc(pa_context *c, const pa_sample_spec *ss)
{
    pa_stream *s = calloc(1, sizeof(*s));
    unsigned i;

    s->ref = 1;
    s->c = c;
    s->idx = (uint32_t)c->next_idx++;
    s->state = PA_STREAM_UNCONNECTED;
    s->ss = *ss;
    s->paused = 1;
    s->alsa_fmt = client_format(ss->format);
    s->alsa_fs = shim_alsa_frame_size(s->alsa_fmt, ss->channels);
    for (i = 0; i < PA_CHANNELS_MAX; i++)
        s->volume[i] = PA_VOLUME_NORM;
    stream_clock_reset(s);
    adjust_attr(s, NULL);
    shim_context_add_stream(c, s);
    c->ref++;
    return s;
}

SHIM_EXPORT
pa_stream *
pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss,
              const pa_channel_map *map)
{
    (void)name;
    (void)map;
    shim_api("pa_stream_new");
    if (!c || !ss)
        return NULL;
    return stream_alloc(c, ss);
}

SHIM_EXPORT
pa_stream *
pa_stream_new_with_proplist(pa_context *c, const char *name,
                            const pa_sample_spec *ss, const pa_channel_map *map,
                            pa_proplist *p)
{
    (void)p;
    shim_api("pa_stream_new_with_proplist");
    return pa_stream_new(c, name, ss, map);
}

SHIM_EXPORT
int
pa_stream_connect_playback(pa_stream *s, const char *dev,
                           const pa_buffer_attr *attr, pa_stream_flags_t flags,
                           const pa_cvolume *volume, pa_stream *sync)
{
    int corked = !!(flags & PA_STREAM_START_CORKED);

    shim_api("pa_stream_connect_playback");
    (void)dev;
    (void)volume;
    (void)sync;
    adjust_attr(s, attr);
    s->paused = corked;
    s->want_running = !corked;
    shim_log("connect corked=%d yield=%d tlength=%u minreq=%u rate=%u\n",
             corked, shim_yield_requested(), s->attr.tlength, s->attr.minreq,
             s->ss.rate);
    /* Corked connect does not open. First play uncorks; that open is
     * the first snd_pcm_open. Opening here is what logged EBUSY while
     * Spotify was still corked (BR8MhQz). */
    if (corked) {
        stream_set_state(s, PA_STREAM_READY);
        return 0;
    }
    if (shim_yield_requested() || stream_open_pcm(s) < 0) {
        stream_set_state(s, PA_STREAM_READY);
        shim_stream_schedule_acquire(s);
        return 0;
    }
    stream_set_state(s, PA_STREAM_READY);
    return 0;
}

static void
cork_run(pa_operation *op)
{
    pa_stream *s = op->s;

    shim_log("cork %d pcm=%d\n", (int)op->idx, s->pcm != NULL);
    if (op->idx) {
        if (s->acquire_ev && s->c->api->time_free) {
            s->c->api->time_free(s->acquire_ev);
            s->acquire_ev = NULL;
        }
        stream_reset_acquire(s);
        s->want_running = 0;
        stream_clock_freeze(s);
        stream_cancel_underflow(s);
        s->paused = 1;
        if (!shim_yield_requested() && s->pcm)
            shim_stream_set_output(s, 1);
        else
            shim_stream_maybe_yield(s);
    } else {
        s->want_running = 1;
        if (s->pcm) {
            stream_clock_start(s);
            s->paused = 0;
            shim_stream_set_output(s, 1);
        } else if (shim_stream_acquire(s) == 0) {
            s->paused = 0;
            stream_clock_start(s);
            shim_stream_set_output(s, 1);
        } else {
            shim_stream_schedule_acquire(s);
        }
    }
    if (op->cb)
        ((pa_stream_success_cb_t)op->cb)(s, 1, op->userdata);
    shim_op_done(op);
}

SHIM_EXPORT
pa_operation *
pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata)
{
    pa_operation *op = shim_op_new(s->c->api, cork_run);

    shim_api("pa_stream_cork");
    op->s = s;
    op->idx = (uint32_t)b;
    op->cb = (void *)cb;
    op->userdata = userdata;
    shim_op_launch(op);
    return op;
}

static void
flush_run(pa_operation *op)
{
    pa_stream *s = op->s;
    snd_pcm_sframes_t av = 0;

    if (s->rb)
        ring_drop(s->rb, ring_readable(s->rb));
    s->timing.write_index = 0;
    s->timing.read_index = 0;
    stream_clock_reset(s);
    stream_cancel_underflow(s);
    s->underflow_sent = 0;
    /*
     * Ring only. snd_pcm_drop/prepare on a "healthy" Motivo MultiRoom
     * handle still kills volumioOutput on a second seek. Committed
     * ALSA audio plays out to the buffer bound.
     */
    if (s->pcm) {
        av = snd_pcm_avail(s->pcm);
        if (s->drop_unsafe ||
            (av < 0 && av != -EAGAIN && av != -EBADFD))
            stream_schedule_reopen(s);
    }
    shim_log("flush ring pcm=%d drop_unsafe=%d avail=%ld\n",
             s->pcm != NULL, s->drop_unsafe, (long)av);
    shim_stream_set_output(s, 0);
    if (op->cb)
        ((pa_stream_success_cb_t)op->cb)(s, 1, op->userdata);
    shim_op_done(op);
}

SHIM_EXPORT
pa_operation *
pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
    pa_operation *op = shim_op_new(s->c->api, flush_run);

    shim_api("pa_stream_flush");
    op->s = s;
    op->cb = (void *)cb;
    op->userdata = userdata;
    shim_op_launch(op);
    return op;
}

SHIM_EXPORT
int
pa_stream_disconnect(pa_stream *s)
{
    shim_api("pa_stream_disconnect");
    if (s->state != PA_STREAM_READY)
        return PA_ERR_BADSTATE;
    shim_log("stream disconnect\n");
    if (s->acquire_ev && s->c->api->time_free) {
        s->c->api->time_free(s->acquire_ev);
        s->acquire_ev = NULL;
    }
    stream_cancel_underflow(s);
    stream_release(s, 0);
    stream_set_state(s, PA_STREAM_TERMINATED);
    return 0;
}

SHIM_EXPORT
uint32_t
pa_stream_get_index(pa_stream *s)
{
    shim_api_hot("pa_stream_get_index");
    return s->idx;
}

SHIM_EXPORT
pa_stream_state_t
pa_stream_get_state(pa_stream *s)
{
    shim_api_hot("pa_stream_get_state");
    return s->state;
}

SHIM_EXPORT
const pa_timing_info *
pa_stream_get_timing_info(pa_stream *s)
{
    shim_api_hot("pa_stream_get_timing_info");
    shim_stream_update_timing(s);
    return &s->timing;
}

static void
update_timing_run(pa_operation *op)
{
    shim_stream_update_timing(op->s);
    if (op->cb)
        ((pa_stream_success_cb_t)op->cb)(op->s, 1, op->userdata);
    shim_op_done(op);
}

SHIM_EXPORT
pa_operation *
pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb,
                             void *userdata)
{
    pa_operation *op = shim_op_new(s->c->api, update_timing_run);

    shim_api_hot("pa_stream_update_timing_info");
    op->s = s;
    op->cb = (void *)cb;
    op->userdata = userdata;
    shim_op_launch(op);
    return op;
}

SHIM_EXPORT
int
pa_stream_is_corked(pa_stream *s)
{
    shim_api_hot("pa_stream_is_corked");
    return s->paused;
}

SHIM_EXPORT
void
pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *u)
{
    shim_api("pa_stream_set_state_callback");
    s->state_cb = cb;
    s->state_cb_userdata = u;
}

SHIM_EXPORT
void
pa_stream_set_started_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *u)
{
    shim_api("pa_stream_set_started_callback");
    s->started_cb = cb;
    s->started_cb_userdata = u;
}

SHIM_EXPORT
void
pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *u)
{
    shim_api("pa_stream_set_underflow_callback");
    s->underflow_cb = cb;
    s->underflow_cb_userdata = u;
}

static size_t
stream_writable_bytes(pa_stream *s)
{
    size_t room_ring, fs;
    int64_t fill, room;

    if (!s || !s->rb)
        return 0;
    room_ring = ring_writable(s->rb);
    fs = shim_frame_size(&s->ss);
    fill = (int64_t)ring_readable(s->rb);
    room = (int64_t)s->attr.tlength - fill;
    if (room < 0)
        room = 0;
    if ((size_t)room < room_ring)
        room_ring = (size_t)room;
    if (fs)
        room_ring -= room_ring % fs;
    return room_ring;
}

SHIM_EXPORT
size_t
pa_stream_writable_size(pa_stream *s)
{
    shim_api_hot("pa_stream_writable_size");
    return stream_writable_bytes(s);
}

SHIM_EXPORT
int
pa_stream_write(pa_stream *s, const void *data, size_t nbytes,
                pa_free_cb_t free_cb, int64_t offset, pa_seek_mode_t seek)
{
    size_t n;

    (void)offset;
    (void)seek;
    shim_api_hot("pa_stream_write");
    if (!s->rb || !data)
        return -1;
    n = ring_write(s->rb, data, nbytes);
    s->timing.write_index += (int64_t)n;
    {
        static int writes, shorts;

        if (n != nbytes && shorts < 8) {
            shim_log("write %zu -> %zu queued=%zu paused=%d pcm=%d\n", nbytes, n,
                     ring_readable(s->rb), s->paused, s->pcm != NULL);
            shorts++;
        } else if (writes < 8) {
            shim_log("write %zu -> %zu queued=%zu paused=%d pcm=%d\n", nbytes, n,
                     ring_readable(s->rb), s->paused, s->pcm != NULL);
            writes++;
        }
    }
    shim_stream_maybe_yield(s);
    if (n > 0) {
        stream_cancel_underflow(s);
        s->underflow_sent = 0;
        if (s->pcm && !s->paused)
            shim_stream_set_output(s, 1);
        else if (!s->pcm && s->want_running) {
            if (shim_stream_acquire(s) == 0) {
                s->paused = 0;
                shim_stream_set_output(s, 1);
            } else {
                shim_stream_schedule_acquire(s);
            }
        }
    }
    if (free_cb)
        free_cb((void *)data);
    return 0;
}

SHIM_EXPORT
void
pa_stream_unref(pa_stream *s)
{
    if (!s)
        return;
    shim_api_hot("pa_stream_unref");
    if (s->pcm && s->ref == 1 && !s->release_pending)
        stream_release(s, 0);
    if (--s->ref > 0)
        return;
    if (s->acquire_ev && s->c->api->time_free)
        s->c->api->time_free(s->acquire_ev);
    stream_cancel_underflow(s);
    shim_context_remove_stream(s->c, s);
    pa_context_unref(s->c);
    ring_free(s->rb);
    free(s->io_buf);
    free(s);
}
