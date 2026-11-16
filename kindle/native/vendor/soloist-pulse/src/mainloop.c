#define _GNU_SOURCE
#include "shim.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
timeval_add_ms(struct timeval *tv, unsigned ms)
{
    tv->tv_usec += (suseconds_t)ms * 1000;
    while (tv->tv_usec >= 1000000) {
        tv->tv_sec++;
        tv->tv_usec -= 1000000;
    }
}

void
shim_wakeup(pa_mainloop *m)
{
    char x = 1;

    if (m && m->wakeup[1] >= 0)
        (void)write(m->wakeup[1], &x, 1);
}

static pa_io_event *
api_io_new(pa_mainloop_api *a, int fd, pa_io_event_flags_t events,
           pa_io_event_cb_t cb, void *userdata)
{
    pa_mainloop *m = a->userdata;
    pa_io_event *e = calloc(1, sizeof(*e));

    e->m = m;
    e->fd = fd;
    e->events = events;
    e->cb = cb;
    e->userdata = userdata;
    m->io = realloc(m->io, (size_t)(m->nio + 1) * sizeof(*m->io));
    m->io[m->nio++] = e;
    shim_wakeup(m);
    return e;
}

static void
api_io_enable(pa_io_event *e, pa_io_event_flags_t events)
{
    e->events = events;
    shim_wakeup(e->m);
}

static void
api_io_free(pa_io_event *e)
{
    pa_mainloop *m = e->m;
    int i;

    for (i = 0; i < m->nio; i++) {
        if (m->io[i] == e) {
            memmove(&m->io[i], &m->io[i + 1],
                    (size_t)(m->nio - i - 1) * sizeof(*m->io));
            m->nio--;
            break;
        }
    }
    free(e);
    shim_wakeup(m);
}

static void
api_io_set_destroy(pa_io_event *e, pa_io_event_destroy_cb_t cb)
{
    (void)e;
    (void)cb;
}

static pa_time_event *
api_time_new(pa_mainloop_api *a, const struct timeval *tv,
             pa_time_event_cb_t cb, void *userdata)
{
    pa_mainloop *m = a->userdata;
    pa_time_event *e = calloc(1, sizeof(*e));

    e->m = m;
    e->when = *tv;
    e->cb = cb;
    e->userdata = userdata;
    m->te = realloc(m->te, (size_t)(m->nte + 1) * sizeof(*m->te));
    m->te[m->nte++] = e;
    shim_wakeup(m);
    return e;
}

static void
api_time_restart(pa_time_event *e, const struct timeval *tv)
{
    e->when = *tv;
    e->dead = 0;
    shim_wakeup(e->m);
}

static void
api_time_free(pa_time_event *e)
{
    e->dead = 1;
    shim_wakeup(e->m);
}

static void
api_time_set_destroy(pa_time_event *e, pa_time_event_destroy_cb_t cb)
{
    (void)e;
    (void)cb;
}

static pa_defer_event *
api_defer_new(pa_mainloop_api *a, pa_defer_event_cb_t cb, void *userdata)
{
    pa_mainloop *m = a->userdata;
    pa_defer_event *e = calloc(1, sizeof(*e));

    e->m = m;
    e->cb = cb;
    e->userdata = userdata;
    e->enabled = 1;
    m->de = realloc(m->de, (size_t)(m->nde + 1) * sizeof(*m->de));
    m->de[m->nde++] = e;
    shim_wakeup(m);
    return e;
}

static void
api_defer_enable(pa_defer_event *e, int b)
{
    e->enabled = b;
    shim_wakeup(e->m);
}

static void
api_defer_free(pa_defer_event *e)
{
    pa_mainloop *m = e->m;
    int i;

    for (i = 0; i < m->nde; i++) {
        if (m->de[i] == e) {
            memmove(&m->de[i], &m->de[i + 1],
                    (size_t)(m->nde - i - 1) * sizeof(*m->de));
            m->nde--;
            break;
        }
    }
    free(e);
}

static void
api_defer_set_destroy(pa_defer_event *e, pa_defer_event_destroy_cb_t cb)
{
    (void)e;
    (void)cb;
}

static void
api_quit(pa_mainloop_api *a, int retval)
{
    pa_mainloop *m = a->userdata;

    m->quit = 1;
    m->retval = retval;
    shim_wakeup(m);
}

static int
from_pa(pa_io_event_flags_t f)
{
    int e = 0;

    if (f & PA_IO_EVENT_INPUT)
        e |= POLLIN;
    if (f & PA_IO_EVENT_OUTPUT)
        e |= POLLOUT;
    return e;
}

static pa_io_event_flags_t
to_pa(int e)
{
    pa_io_event_flags_t f = 0;

    if (e & POLLIN)
        f |= PA_IO_EVENT_INPUT;
    if (e & POLLOUT)
        f |= PA_IO_EVENT_OUTPUT;
    if (e & (POLLERR | POLLHUP | POLLNVAL))
        f |= PA_IO_EVENT_ERROR;
    return f;
}

static void
collect_dead_times(pa_mainloop *m)
{
    int i = 0;

    while (i < m->nte) {
        if (m->te[i]->dead) {
            free(m->te[i]);
            memmove(&m->te[i], &m->te[i + 1],
                    (size_t)(m->nte - i - 1) * sizeof(*m->te));
            m->nte--;
        } else {
            i++;
        }
    }
}

static int
io_registered(pa_mainloop *m, pa_io_event *e)
{
    int i;

    for (i = 0; i < m->nio; i++)
        if (m->io[i] == e)
            return 1;
    return 0;
}

static int
defer_registered(pa_mainloop *m, pa_defer_event *e)
{
    int i;

    for (i = 0; i < m->nde; i++)
        if (m->de[i] == e)
            return 1;
    return 0;
}

static void
dispatch_defers(pa_mainloop *m)
{
    pa_defer_event **dsnap = NULL;
    int i, nde;

    nde = m->nde;
    if (nde > 0) {
        dsnap = malloc((size_t)nde * sizeof(*dsnap));
        if (dsnap)
            memcpy(dsnap, m->de, (size_t)nde * sizeof(*dsnap));
    }
    m->in_dispatch = 1;
    for (i = 0; i < nde && dsnap; i++) {
        pa_defer_event *e = dsnap[i];

        if (defer_registered(m, e) && e->enabled && e->cb)
            e->cb(&m->api, e, e->userdata);
    }
    m->in_dispatch = 0;
    free(dsnap);
}

static int
mainloop_iterate(pa_mainloop *m, int block)
{
    struct pollfd *fds;
    pa_io_event **isnap = NULL;
    int nfds, i, timeout, n, nio;
    struct timeval now;

    shim_finish_pending_releases(m);
    collect_dead_times(m);
    dispatch_defers(m);
    shim_finish_pending_releases(m);

    if (m->quit)
        return -2;

    nfds = 1 + m->nio;
    fds = calloc((size_t)nfds, sizeof(*fds));
    fds[0].fd = m->wakeup[0];
    fds[0].events = POLLIN;
    for (i = 0; i < m->nio; i++) {
        fds[i + 1].fd = m->io[i]->fd;
        fds[i + 1].events = from_pa(m->io[i]->events);
        m->io[i]->pollfd = &fds[i + 1];
    }

    timeout = block ? 25 : 0;
    gettimeofday(&now, NULL);
    for (i = 0; i < m->nte; i++) {
        pa_time_event *e = m->te[i];
        long ms;

        if (e->dead)
            continue;
        ms = (e->when.tv_sec - now.tv_sec) * 1000 +
             (e->when.tv_usec - now.tv_usec) / 1000;
        if (ms < 0)
            ms = 0;
        if (timeout < 0 || ms < timeout)
            timeout = (int)ms;
    }

    if (m->poll_lock)
        pthread_mutex_unlock(m->poll_lock);
    m->in_poll = 1;
    n = poll(fds, (nfds_t)nfds, timeout);
    m->in_poll = 0;
    if (m->poll_lock)
        pthread_mutex_lock(m->poll_lock);

    if (n < 0 && errno != EINTR) {
        free(fds);
        return -1;
    }

    if (fds[0].revents & POLLIN) {
        char buf[64];

        while (read(m->wakeup[0], buf, sizeof(buf)) > 0) {
        }
    }

    shim_finish_pending_releases(m);
    dispatch_defers(m);
    if (m->quit) {
        free(fds);
        return -2;
    }

    gettimeofday(&now, NULL);
    for (i = 0; i < m->nte; i++) {
        pa_time_event *e = m->te[i];

        if (e->dead || !e->cb)
            continue;
        if (e->when.tv_sec < now.tv_sec ||
            (e->when.tv_sec == now.tv_sec && e->when.tv_usec <= now.tv_usec)) {
            e->cb(&m->api, e, &e->when, e->userdata);
        }
    }

    nio = m->nio;
    if (nio > 0) {
        isnap = malloc((size_t)nio * sizeof(*isnap));
        memcpy(isnap, m->io, (size_t)nio * sizeof(*isnap));
    }
    m->in_dispatch = 1;
    for (i = 0; i < nio; i++) {
        pa_io_event *e = isnap[i];
        unsigned short revents = 0;

        if (!io_registered(m, e) || !e->pollfd)
            continue;
        if (e->pcm)
            snd_pcm_poll_descriptors_revents(e->pcm, e->pollfd, 1, &revents);
        else
            revents = e->pollfd->revents;
        /*
         * An error on a PCM descriptor is an underrun on hardware, a
         * hangup or a dead fd. The stream recovers it, with the position
         * sampled and the line in the diag; a bare prepare here would
         * leave the device PREPARED with nothing said and nothing told.
         */
        if (revents & ~(POLLOUT | POLLIN)) {
            if (e->pcm && e->userdata)
                shim_stream_poll_error(e->userdata);
            else if (e->pcm)
                snd_pcm_prepare(e->pcm);
        }
        if ((revents & (POLLOUT | POLLIN | POLLERR | POLLHUP)) && e->cb)
            e->cb(&m->api, e, e->fd, to_pa((int)revents), e->userdata);
    }
    m->in_dispatch = 0;
    free(isnap);
    shim_finish_pending_releases(m);

    free(fds);
    return n > 0 ? 1 : 0;
}

static void *
mainloop_thread(void *arg)
{
    pa_threaded_mainloop *tm = arg;

    tm->m->thread = pthread_self();
    tm->m->have_thread = 1;
    pthread_mutex_lock(&tm->lock);
    while (!tm->m->quit)
        mainloop_iterate(tm->m, 1);
    shim_finish_pending_releases(tm->m);
    pthread_mutex_unlock(&tm->lock);
    return NULL;
}

SHIM_EXPORT
pa_threaded_mainloop *
pa_threaded_mainloop_new(void)
{
    pa_threaded_mainloop *tm;

    shim_api("pa_threaded_mainloop_new");
    tm = calloc(1, sizeof(*tm));
    pa_mainloop *m = calloc(1, sizeof(*m));
    pthread_mutexattr_t attr;

    if (pipe(m->wakeup) < 0) {
        free(m);
        free(tm);
        return NULL;
    }
    fcntl(m->wakeup[0], F_SETFL, O_NONBLOCK);
    fcntl(m->wakeup[1], F_SETFL, O_NONBLOCK);
    m->api.userdata = m;
    m->api.io_new = api_io_new;
    m->api.io_enable = api_io_enable;
    m->api.io_free = api_io_free;
    m->api.io_set_destroy = api_io_set_destroy;
    m->api.time_new = api_time_new;
    m->api.time_restart = api_time_restart;
    m->api.time_free = api_time_free;
    m->api.time_set_destroy = api_time_set_destroy;
    m->api.defer_new = api_defer_new;
    m->api.defer_enable = api_defer_enable;
    m->api.defer_free = api_defer_free;
    m->api.defer_set_destroy = api_defer_set_destroy;
    m->api.quit = api_quit;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&tm->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&tm->cond, NULL);
    m->poll_lock = &tm->lock;
    tm->m = m;
    return tm;
}

SHIM_EXPORT
void
pa_threaded_mainloop_free(pa_threaded_mainloop *tm)
{
    pa_mainloop *m;
    int i;

    if (!tm)
        return;
    shim_api("pa_threaded_mainloop_free");
    if (tm->running)
        pa_threaded_mainloop_stop(tm);
    if (tm->abandoned) {
        free(tm);
        return;
    }
    m = tm->m;
    for (i = 0; i < m->nio; i++)
        free(m->io[i]);
    free(m->io);
    for (i = 0; i < m->nte; i++)
        free(m->te[i]);
    free(m->te);
    for (i = 0; i < m->nde; i++)
        free(m->de[i]);
    free(m->de);
    close(m->wakeup[0]);
    close(m->wakeup[1]);
    free(m);
    pthread_mutex_destroy(&tm->lock);
    pthread_cond_destroy(&tm->cond);
    free(tm);
}

SHIM_EXPORT
pa_mainloop_api *
pa_threaded_mainloop_get_api(pa_threaded_mainloop *tm)
{
    shim_api("pa_threaded_mainloop_get_api");
    return tm ? &tm->m->api : NULL;
}

SHIM_EXPORT
void
pa_threaded_mainloop_lock(pa_threaded_mainloop *tm)
{
    if (!tm)
        return;
    shim_api_hot("pa_threaded_mainloop_lock");
    pthread_mutex_lock(&tm->lock);
    tm->lock_depth++;
}

SHIM_EXPORT
void
pa_threaded_mainloop_unlock(pa_threaded_mainloop *tm)
{
    if (!tm || tm->lock_depth <= 0)
        return;
    shim_api_hot("pa_threaded_mainloop_unlock");
    tm->lock_depth--;
    pthread_mutex_unlock(&tm->lock);
}

SHIM_EXPORT
void
pa_threaded_mainloop_signal(pa_threaded_mainloop *tm, int wait_for_accept)
{
    shim_api_hot("pa_threaded_mainloop_signal");
    (void)wait_for_accept;
    if (tm)
        pthread_cond_signal(&tm->cond);
}

SHIM_EXPORT
void
pa_threaded_mainloop_wait(pa_threaded_mainloop *tm)
{
    shim_api_hot("pa_threaded_mainloop_wait");
    if (tm)
        pthread_cond_wait(&tm->cond, &tm->lock);
}

SHIM_EXPORT
int
pa_threaded_mainloop_start(pa_threaded_mainloop *tm)
{
    shim_api("pa_threaded_mainloop_start");
    if (!tm || tm->running)
        return tm ? 1 : -1;
    if (pthread_create(&tm->thread, NULL, mainloop_thread, tm) != 0)
        return -1;
    tm->running = 1;
    return 0;
}

SHIM_EXPORT
void
pa_threaded_mainloop_stop(pa_threaded_mainloop *tm)
{
    int depth;

    if (!tm || !tm->running)
        return;
    shim_log("mainloop stop\n");
    if (tm->m->have_thread && pthread_equal(pthread_self(), tm->m->thread)) {
        tm->m->quit = 1;
        shim_wakeup(tm->m);
        return;
    }
    tm->m->quit = 1;
    shim_wakeup(tm->m);
    depth = tm->lock_depth;
    while (tm->lock_depth > 0)
        pa_threaded_mainloop_unlock(tm);
    {
        struct timespec ts;
        int jr;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        jr = pthread_timedjoin_np(tm->thread, NULL, &ts);
        if (jr != 0) {
            shim_log("mainloop join timed out (%d), abandoning thread\n", jr);
            tm->abandoned = 1;
            tm->running = 0;
            return;
        }
    }
    tm->running = 0;
    tm->m->have_thread = 0;
    shim_finish_pending_releases(tm->m);
    while (tm->lock_depth < depth)
        pa_threaded_mainloop_lock(tm);
}

static void
op_defer(pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
    pa_operation *op = userdata;

    (void)a;
    a->defer_free(e);
    if (op->run)
        op->run(op);
}

void
shim_op_launch(pa_operation *op)
{
    if (!op || !op->api || !op->api->defer_new)
        return;
    op->ref++;
    op->api->defer_new(op->api, op_defer, op);
}

pa_time_event *
shim_after_ms(pa_mainloop_api *api, unsigned ms, pa_time_event_cb_t cb,
              void *userdata)
{
    struct timeval tv;

    if (!api || !api->time_new)
        return NULL;
    gettimeofday(&tv, NULL);
    timeval_add_ms(&tv, ms);
    return api->time_new(api, &tv, cb, userdata);
}
