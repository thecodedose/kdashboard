#pragma once

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <pulse/pulseaudio.h>
#include <stddef.h>
#include <stdint.h>

#define SHIM_EXPORT __attribute__((visibility("default")))

#define SHIM_PATH_MAX 768

/* A dry ring is reported as an underflow once ALSA has played out what it
 * held. Each wait is the delay the PCM reports plus this margin; a PCM
 * that will not say gets the whole buffer. A spell ends at the latest
 * after twice the configured buffer plus the margin. */
#define SHIM_UNDERFLOW_MARGIN_MS 20

struct ring;

struct pa_io_event {
    pa_mainloop *m;
    int fd;
    pa_io_event_flags_t events;
    pa_io_event_cb_t cb;
    void *userdata;
    snd_pcm_t *pcm;
    struct pollfd *pollfd;
};

struct pa_time_event {
    pa_mainloop *m;
    struct timeval when;
    pa_time_event_cb_t cb;
    void *userdata;
    int dead;
};

struct pa_defer_event {
    pa_mainloop *m;
    pa_defer_event_cb_t cb;
    void *userdata;
    int enabled;
};

struct pa_mainloop {
    pa_mainloop_api api;
    pthread_mutex_t *poll_lock;
    int wakeup[2];
    int quit;
    int retval;
    pa_io_event **io;
    int nio;
    pa_time_event **te;
    int nte;
    pa_defer_event **de;
    int nde;
    pthread_t thread;
    int have_thread;
    int in_poll;
    int in_dispatch;
    pa_stream **pending_release;
    int npending;
};

struct pa_threaded_mainloop {
    pa_mainloop *m;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    int running;
    int lock_depth;
    int abandoned;
};

struct pa_proplist {
    char **k;
    char **v;
    size_t n;
};

struct pa_operation {
    int ref;
    pa_operation_state_t state;
    pa_mainloop_api *api;
    void (*run)(struct pa_operation *);
    pa_context *c;
    pa_stream *s;
    void *cb;
    void *userdata;
    uint32_t idx;
    pa_cvolume volume;
};

struct pa_context {
    int ref;
    pa_context_state_t state;
    pa_mainloop_api *api;
    char *name;
    pa_context_notify_cb_t state_cb;
    void *state_cb_userdata;
    pa_context_subscribe_cb_t sub_cb;
    void *sub_cb_userdata;
    pa_stream **streams;
    int nstreams;
    int next_idx;
};

struct pa_stream {
    int ref;
    pa_context *c;
    uint32_t idx;
    pa_stream_state_t state;
    pa_sample_spec ss;
    pa_buffer_attr attr;
    pa_timing_info timing;
    snd_pcm_t *pcm;
    struct ring *rb;
    char *io_buf;
    size_t io_buf_bytes;
    snd_pcm_uframes_t period;
    pa_io_event **ioe;
    int nioe;
    pa_io_event_flags_t ioe_events;
    int out_enabled;
    volatile int paused;
    int want_running;
    pa_volume_t volume[PA_CHANNELS_MAX];
    pa_stream_notify_cb_t state_cb;
    void *state_cb_userdata;
    pa_stream_notify_cb_t started_cb;
    void *started_cb_userdata;
    pa_stream_notify_cb_t underflow_cb;
    void *underflow_cb_userdata;
    pa_usec_t configured_sink_usec;
    snd_pcm_format_t alsa_fmt;
    size_t alsa_fs;
    int64_t clock_frozen_bytes;
    int64_t clock_last_played_bytes;
    int clock_running;
    pa_time_event *acquire_ev;
    int acquire_attempts;
    int busy_waits;
    int busy_wait_logged;
    int busy_fail_logged;
    int release_pending;
    int release_keep_position;
    int reopen_pending;
    int reopen_wait;
    int drop_unsafe;
    pa_time_event *underflow_ev;
    int underflow_sent;
    struct timeval dry_since;
    unsigned dry_wait_ms;
    snd_pcm_sframes_t dry_last_delay;
};

struct ring *ring_new(size_t bytes);
void ring_free(struct ring *r);
size_t ring_capacity(struct ring *r);
size_t ring_readable(struct ring *r);
size_t ring_writable(struct ring *r);
size_t ring_write(struct ring *r, const void *data, size_t n);
size_t ring_read(struct ring *r, void *data, size_t n);
size_t ring_peek(struct ring *r, void *data, size_t n);
void ring_drop(struct ring *r, size_t n);

void shim_log(const char *fmt, ...);
void shim_scrub_cmdline_secrets(void);
void shim_api(const char *fn);
void shim_api_hot(const char *fn);
void shim_wakeup(pa_mainloop *m);
void shim_finish_pending_releases(pa_mainloop *m);
void shim_op_launch(pa_operation *op);
pa_operation *shim_op_new(pa_mainloop_api *api, void (*run)(pa_operation *));
void shim_op_done(pa_operation *op);
int shim_yield_requested(void);
const char *shim_playback_device(void);
int shim_external_volume(void);
float shim_trim_gain(void);
void shim_apply_volume(void *buf, size_t bytes, const pa_volume_t *vol,
                       const pa_sample_spec *ss);
void shim_apply_trim(void *buf, size_t bytes, const pa_sample_spec *ss);
size_t shim_frame_size(const pa_sample_spec *ss);
size_t shim_alsa_frame_size(snd_pcm_format_t fmt, unsigned channels);
void shim_convert_to_alsa(void *buf, size_t frames, unsigned channels,
                          pa_sample_format_t from, snd_pcm_format_t to);
const char *shim_alsa_format_name(snd_pcm_format_t fmt);
void shim_context_add_stream(pa_context *c, pa_stream *s);
void shim_context_remove_stream(pa_context *c, pa_stream *s);
pa_stream *shim_context_find_stream(pa_context *c, uint32_t idx);

void shim_stream_maybe_yield(pa_stream *s);
void shim_stream_update_timing(pa_stream *s);
void shim_stream_set_output(pa_stream *s, int enable);
void shim_stream_poll_error(pa_stream *s);
void shim_stream_schedule_acquire(pa_stream *s);
int shim_stream_acquire(pa_stream *s);
pa_time_event *shim_after_ms(pa_mainloop_api *api, unsigned ms,
                             pa_time_event_cb_t cb, void *userdata);
