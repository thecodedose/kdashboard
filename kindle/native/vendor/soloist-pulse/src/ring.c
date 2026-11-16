#include "shim.h"

#include <stdlib.h>
#include <string.h>

struct ring {
    char *buf;
    char *end;
    char *r;
    char *w;
    pthread_mutex_t lock;
    int empty;
};

struct ring *
ring_new(size_t bytes)
{
    struct ring *r = calloc(1, sizeof(*r));

    if (!r)
        return NULL;
    r->buf = malloc(bytes);
    if (!r->buf) {
        free(r);
        return NULL;
    }
    r->end = r->buf + bytes;
    r->r = r->w = r->buf;
    r->empty = 1;
    pthread_mutex_init(&r->lock, NULL);
    return r;
}

void
ring_free(struct ring *r)
{
    if (!r)
        return;
    pthread_mutex_destroy(&r->lock);
    free(r->buf);
    free(r);
}

size_t
ring_capacity(struct ring *r)
{
    if (!r || !r->buf)
        return 0;
    return (size_t)(r->end - r->buf);
}

static size_t
readable_unlocked(struct ring *r)
{
    if (r->w > r->r)
        return (size_t)(r->w - r->r);
    if (r->w < r->r)
        return (size_t)(r->end - r->r) + (size_t)(r->w - r->buf);
    return r->empty ? 0 : (size_t)(r->end - r->buf);
}

static size_t
writable_unlocked(struct ring *r)
{
    return (size_t)(r->end - r->buf) - readable_unlocked(r);
}

size_t
ring_readable(struct ring *r)
{
    size_t n;

    pthread_mutex_lock(&r->lock);
    n = readable_unlocked(r);
    pthread_mutex_unlock(&r->lock);
    return n;
}

size_t
ring_writable(struct ring *r)
{
    size_t n;

    pthread_mutex_lock(&r->lock);
    n = writable_unlocked(r);
    pthread_mutex_unlock(&r->lock);
    return n;
}

size_t
ring_write(struct ring *r, const void *data, size_t n)
{
    const char *p = data;
    size_t left, chunk;

    pthread_mutex_lock(&r->lock);
    left = writable_unlocked(r);
    if (n > left)
        n = left;
    left = n;
    while (left) {
        chunk = (size_t)(r->end - r->w);
        if (chunk > left)
            chunk = left;
        memcpy(r->w, p, chunk);
        r->w += chunk;
        if (r->w == r->end)
            r->w = r->buf;
        p += chunk;
        left -= chunk;
    }
    if (n)
        r->empty = 0;
    pthread_mutex_unlock(&r->lock);
    return n;
}

size_t
ring_read(struct ring *r, void *data, size_t n)
{
    char *p = data;
    size_t left, chunk;

    pthread_mutex_lock(&r->lock);
    left = readable_unlocked(r);
    if (n > left)
        n = left;
    left = n;
    while (left) {
        chunk = (size_t)(r->end - r->r);
        if (chunk > left)
            chunk = left;
        memcpy(p, r->r, chunk);
        r->r += chunk;
        if (r->r == r->end)
            r->r = r->buf;
        p += chunk;
        left -= chunk;
    }
    if (r->r == r->w)
        r->empty = 1;
    pthread_mutex_unlock(&r->lock);
    return n;
}

size_t
ring_peek(struct ring *r, void *data, size_t n)
{
    char *p = data;
    char *rp;
    size_t left, chunk, avail;

    pthread_mutex_lock(&r->lock);
    avail = readable_unlocked(r);
    if (n > avail)
        n = avail;
    rp = r->r;
    left = n;
    while (left) {
        chunk = (size_t)(r->end - rp);
        if (chunk > left)
            chunk = left;
        memcpy(p, rp, chunk);
        rp += chunk;
        if (rp == r->end)
            rp = r->buf;
        p += chunk;
        left -= chunk;
    }
    pthread_mutex_unlock(&r->lock);
    return n;
}

void
ring_drop(struct ring *r, size_t n)
{
    size_t left, chunk;

    pthread_mutex_lock(&r->lock);
    left = readable_unlocked(r);
    if (n > left)
        n = left;
    left = n;
    while (left) {
        chunk = (size_t)(r->end - r->r);
        if (chunk > left)
            chunk = left;
        r->r += chunk;
        if (r->r == r->end)
            r->r = r->buf;
        left -= chunk;
    }
    if (r->r == r->w)
        r->empty = 1;
    pthread_mutex_unlock(&r->lock);
}
