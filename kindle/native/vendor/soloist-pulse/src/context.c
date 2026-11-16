#include "shim.h"

#include <stdlib.h>
#include <string.h>

void
shim_context_add_stream(pa_context *c, pa_stream *s)
{
    c->streams = realloc(c->streams, (size_t)(c->nstreams + 1) * sizeof(*c->streams));
    c->streams[c->nstreams++] = s;
}

void
shim_context_remove_stream(pa_context *c, pa_stream *s)
{
    int i;

    for (i = 0; i < c->nstreams; i++) {
        if (c->streams[i] == s) {
            memmove(&c->streams[i], &c->streams[i + 1],
                    (size_t)(c->nstreams - i - 1) * sizeof(*c->streams));
            c->nstreams--;
            return;
        }
    }
}

pa_stream *
shim_context_find_stream(pa_context *c, uint32_t idx)
{
    int i;

    for (i = 0; i < c->nstreams; i++)
        if (c->streams[i]->idx == idx)
            return c->streams[i];
    return NULL;
}

static void
context_set_state(pa_context *c, pa_context_state_t st)
{
    if (c->state == st)
        return;
    c->ref++;
    c->state = st;
    if (c->state_cb)
        c->state_cb(c, c->state_cb_userdata);
    pa_context_unref(c);
}

SHIM_EXPORT
pa_context *
pa_context_new(pa_mainloop_api *api, const char *name)
{
    pa_context *c = calloc(1, sizeof(*c));

    shim_api("pa_context_new");
    c->ref = 1;
    c->state = PA_CONTEXT_UNCONNECTED;
    c->api = api;
    c->name = name ? strdup(name) : NULL;
    c->next_idx = 1;
    return c;
}

SHIM_EXPORT
int
pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags,
                   const pa_spawn_api *api)
{
    (void)server;
    (void)flags;
    (void)api;
    shim_api("pa_context_connect");
    context_set_state(c, PA_CONTEXT_CONNECTING);
    context_set_state(c, PA_CONTEXT_READY);
    shim_log("context ready\n");
    return 0;
}

SHIM_EXPORT
void
pa_context_disconnect(pa_context *c)
{
    pa_stream **snap;
    int i, n;

    shim_api("pa_context_disconnect");
    n = c->nstreams;
    snap = n > 0 ? malloc((size_t)n * sizeof(*snap)) : NULL;
    if (snap)
        memcpy(snap, c->streams, (size_t)n * sizeof(*snap));
    for (i = 0; i < n; i++) {
        pa_stream *s = snap[i];

        s->ref++;
        if (s->state == PA_STREAM_READY)
            pa_stream_disconnect(s);
        pa_stream_unref(s);
    }
    free(snap);
    context_set_state(c, PA_CONTEXT_TERMINATED);
}

SHIM_EXPORT
int
pa_context_errno(pa_context *c)
{
    (void)c;
    shim_api_hot("pa_context_errno");
    return 0;
}

SHIM_EXPORT
pa_context_state_t
pa_context_get_state(pa_context *c)
{
    shim_api_hot("pa_context_get_state");
    return c ? c->state : PA_CONTEXT_UNCONNECTED;
}

SHIM_EXPORT
void
pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb,
                              void *userdata)
{
    shim_api("pa_context_set_state_callback");
    c->state_cb = cb;
    c->state_cb_userdata = userdata;
}

SHIM_EXPORT
void
pa_context_set_subscribe_callback(pa_context *c, pa_context_subscribe_cb_t cb,
                                  void *userdata)
{
    shim_api("pa_context_set_subscribe_callback");
    c->sub_cb = cb;
    c->sub_cb_userdata = userdata;
}

static void
subscribe_run(pa_operation *op)
{
    shim_log("subscribe op done\n");
    if (op->c->sub_cb)
        op->c->sub_cb(op->c, PA_SUBSCRIPTION_EVENT_SINK_INPUT, 0,
                      op->c->sub_cb_userdata);
    if (op->cb)
        ((pa_context_success_cb_t)op->cb)(op->c, 1, op->userdata);
    shim_op_done(op);
}

SHIM_EXPORT
pa_operation *
pa_context_subscribe(pa_context *c, pa_subscription_mask_t m,
                     pa_context_success_cb_t cb, void *userdata)
{
    pa_operation *op = shim_op_new(c->api, subscribe_run);

    shim_api("pa_context_subscribe");
    (void)m;
    op->c = c;
    op->cb = (void *)cb;
    op->userdata = userdata;
    shim_op_launch(op);
    return op;
}

static void
sink_input_run(pa_operation *op)
{
    pa_sink_input_info info;
    pa_stream *s = shim_context_find_stream(op->c, op->idx);

    memset(&info, 0, sizeof(info));
    info.index = op->idx;
    info.name = "soloist";
    info.owner_module = PA_INVALID_INDEX;
    info.client = PA_INVALID_INDEX;
    info.sink = 0;
    info.sample_spec.format = PA_SAMPLE_FLOAT32LE;
    info.sample_spec.rate = 44100;
    info.sample_spec.channels = 2;
    info.channel_map.channels = 2;
    info.channel_map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
    info.channel_map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
    info.volume.channels = 2;
    info.volume.values[0] = PA_VOLUME_NORM;
    info.volume.values[1] = PA_VOLUME_NORM;
    if (s) {
        info.sample_spec = s->ss;
        info.corked = s->paused;
        info.volume.channels = s->ss.channels;
        {
            unsigned i;

            for (i = 0; i < s->ss.channels && i < PA_CHANNELS_MAX; i++)
                info.volume.values[i] = s->volume[i];
        }
    }
    if (op->cb)
        ((pa_sink_input_info_cb_t)op->cb)(op->c, &info, 0, op->userdata);
    if (op->cb)
        ((pa_sink_input_info_cb_t)op->cb)(op->c, NULL, 1, op->userdata);
    shim_op_done(op);
}

SHIM_EXPORT
pa_operation *
pa_context_get_sink_input_info(pa_context *c, uint32_t idx,
                               pa_sink_input_info_cb_t cb, void *userdata)
{
    pa_operation *op = shim_op_new(c->api, sink_input_run);

    shim_api("pa_context_get_sink_input_info");
    op->c = c;
    op->idx = idx;
    op->cb = (void *)cb;
    op->userdata = userdata;
    shim_op_launch(op);
    return op;
}

static void
set_volume_run(pa_operation *op)
{
    pa_stream *s = shim_context_find_stream(op->c, op->idx);
    unsigned i;

    if (s) {
        for (i = 0; i < op->volume.channels && i < PA_CHANNELS_MAX; i++)
            s->volume[i] = op->volume.values[i];
    }
    if (op->cb)
        ((pa_context_success_cb_t)op->cb)(op->c, 1, op->userdata);
    shim_op_done(op);
}

SHIM_EXPORT
pa_operation *
pa_context_set_sink_input_volume(pa_context *c, uint32_t idx,
                                 const pa_cvolume *v,
                                 pa_context_success_cb_t cb, void *userdata)
{
    pa_operation *op = shim_op_new(c->api, set_volume_run);

    shim_api("pa_context_set_sink_input_volume");
    op->c = c;
    op->idx = idx;
    if (v)
        op->volume = *v;
    op->cb = (void *)cb;
    op->userdata = userdata;
    shim_op_launch(op);
    return op;
}

SHIM_EXPORT
void
pa_context_unref(pa_context *c)
{
    if (!c)
        return;
    shim_api_hot("pa_context_unref");
    if (--c->ref > 0)
        return;
    free(c->name);
    free(c->streams);
    free(c);
}
