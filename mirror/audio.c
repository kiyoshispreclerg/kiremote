/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio.h"

#include <errno.h>
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "log.h"

#define MAX_FDS 128

struct kr_audio {
    pa_mainloop *ml;
    pa_context *ctx;
    pa_stream *st;
    char title[128];
    uint8_t *q; /* PCM waiting for room in the stream */
    size_t q_len, q_cap;
    bool failed;
    /* Of the poll() in progress. */
    struct pollfd *extra;
    nfds_t n_extra;
    int extra_timeout, rc, rc_errno;
};

static void drain(kr_audio *a)
{
    if (!a->st || pa_stream_get_state(a->st) != PA_STREAM_READY || !a->q_len)
        return;
    size_t room = pa_stream_writable_size(a->st);
    if (room == (size_t)-1)
        return;
    size_t n = a->q_len < room ? a->q_len : room;
    n -= n % KR_AUDIO_FRAME;
    if (!n)
        return;
    if (pa_stream_write(a->st, a->q, n, NULL, 0, PA_SEEK_RELATIVE) < 0) {
        kr_log_warn(_("Sound: %s"), pa_strerror(pa_context_errno(a->ctx)));
        a->failed = true;
        return;
    }
    memmove(a->q, a->q + n, a->q_len - n);
    a->q_len -= n;
}

static void stream_state(pa_stream *s, void *ud)
{
    kr_audio *a = ud;
    pa_stream_state_t st = pa_stream_get_state(s);
    if (st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) {
        if (st == PA_STREAM_FAILED && !a->failed)
            kr_log_warn(_("Sound: %s"), pa_strerror(pa_context_errno(a->ctx)));
        a->failed = true;
    } else if (st == PA_STREAM_READY) {
        drain(a);
    }
}

static void stream_write(pa_stream *s, size_t nbytes, void *ud)
{
    (void)s;
    (void)nbytes;
    drain(ud);
}

static void start_stream(kr_audio *a)
{
    static const pa_sample_spec ss = {.format = PA_SAMPLE_S16LE, .rate = KR_AUDIO_RATE, .channels = 2};
    a->st = pa_stream_new(a->ctx, a->title, &ss, NULL);
    if (!a->st) {
        a->failed = true;
        return;
    }
    pa_stream_set_state_callback(a->st, stream_state, a);
    pa_stream_set_write_callback(a->st, stream_write, a);
    /* About 80 ms ahead in the server: enough to ride out a late packet,
     * little enough to stay in step with the picture. */
    pa_buffer_attr ba = {.maxlength = (uint32_t)-1,
                         .tlength = (uint32_t)pa_usec_to_bytes(80 * PA_USEC_PER_MSEC, &ss),
                         .prebuf = 0, /* start at once; a late packet is a short gap, not a wait */
                         .minreq = (uint32_t)-1,
                         .fragsize = (uint32_t)-1};
    if (pa_stream_connect_playback(a->st, NULL, &ba, PA_STREAM_ADJUST_LATENCY | PA_STREAM_INTERPOLATE_TIMING,
                                   NULL, NULL) < 0) {
        kr_log_warn(_("Sound: %s"), pa_strerror(pa_context_errno(a->ctx)));
        a->failed = true;
    }
}

static void context_state(pa_context *c, void *ud)
{
    kr_audio *a = ud;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        start_stream(a);
        break;
    case PA_CONTEXT_FAILED:
        if (!a->failed)
            kr_log_warn(_("Sound: %s"), pa_strerror(pa_context_errno(c)));
        a->failed = true;
        break;
    case PA_CONTEXT_TERMINATED:
        a->failed = true; /* by us, in kr_audio_free */
        break;
    default:
        break;
    }
}

kr_audio *kr_audio_new(const char *app, const char *title, const char **why)
{
    kr_audio *a = calloc(1, sizeof *a);
    if (!a) {
        *why = _("Out of memory");
        return NULL;
    }
    snprintf(a->title, sizeof a->title, "%s", title);
    a->ml = pa_mainloop_new();
    a->ctx = a->ml ? pa_context_new(pa_mainloop_get_api(a->ml), app) : NULL;
    if (!a->ctx || pa_context_connect(a->ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        *why = a->ctx ? pa_strerror(pa_context_errno(a->ctx)) : _("Out of memory");
        kr_audio_free(a);
        return NULL;
    }
    pa_context_set_state_callback(a->ctx, context_state, a);
    return a;
}

void kr_audio_free(kr_audio *a)
{
    if (!a)
        return;
    if (a->st) {
        pa_stream_set_state_callback(a->st, NULL, NULL);
        pa_stream_set_write_callback(a->st, NULL, NULL);
        pa_stream_unref(a->st);
    }
    if (a->ctx) {
        pa_context_set_state_callback(a->ctx, NULL, NULL);
        pa_context_disconnect(a->ctx);
        pa_context_unref(a->ctx);
    }
    if (a->ml)
        pa_mainloop_free(a->ml);
    free(a->q);
    free(a);
}

bool kr_audio_ok(const kr_audio *a)
{
    return !a->failed;
}

void kr_audio_play(kr_audio *a, const uint8_t *pcm, size_t len)
{
    if (a->failed || !len)
        return;
    if (len > KR_AUDIO_MAX_QUEUE) { /* only the end of it is still timely */
        pcm += len - KR_AUDIO_MAX_QUEUE;
        len = KR_AUDIO_MAX_QUEUE;
    }
    if (a->q_len + len > KR_AUDIO_MAX_QUEUE) { /* what waits longest goes */
        size_t drop = a->q_len + len - KR_AUDIO_MAX_QUEUE;
        if (drop > a->q_len)
            drop = a->q_len;
        drop += (KR_AUDIO_FRAME - drop % KR_AUDIO_FRAME) % KR_AUDIO_FRAME;
        if (drop > a->q_len)
            drop = a->q_len;
        memmove(a->q, a->q + drop, a->q_len - drop);
        a->q_len -= drop;
    }
    if (a->q_len + len > a->q_cap) {
        size_t cap = a->q_cap ? a->q_cap : 4096;
        while (cap < a->q_len + len)
            cap *= 2;
        uint8_t *b = realloc(a->q, cap);
        if (!b)
            return;
        a->q = b;
        a->q_cap = cap;
    }
    memcpy(a->q + a->q_len, pcm, len);
    a->q_len += len;
    drain(a);
}

size_t kr_audio_queued(const kr_audio *a)
{
    return a->q_len;
}

/* libpulse's poll hook: one poll() for its descriptors and the caller's. */
static int poll_hook(struct pollfd *ufds, unsigned long nfds, int timeout, void *ud)
{
    kr_audio *a = ud;
    struct pollfd all[MAX_FDS];
    if (nfds > MAX_FDS - a->n_extra) /* a handful is what libpulse uses */
        nfds = MAX_FDS - a->n_extra;
    memcpy(all, ufds, nfds * sizeof *all);
    memcpy(all + nfds, a->extra, a->n_extra * sizeof *all);
    int t = a->extra_timeout;
    if (timeout >= 0 && (t < 0 || timeout < t))
        t = timeout;
    int r = poll(all, nfds + a->n_extra, t);
    a->rc_errno = errno;
    memcpy(ufds, all, nfds * sizeof *all);
    memcpy(a->extra, all + nfds, a->n_extra * sizeof *all);
    a->rc = r;
    return r;
}

int kr_audio_poll(kr_audio *a, struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    if (a->failed || nfds > MAX_FDS - 16)
        return poll(fds, nfds, timeout_ms);
    a->extra = fds;
    a->n_extra = nfds;
    a->extra_timeout = timeout_ms;
    a->rc = 0;
    a->rc_errno = 0;
    pa_mainloop_set_poll_func(a->ml, poll_hook, a);
    if (pa_mainloop_prepare(a->ml, -1) < 0)
        return poll(fds, nfds, timeout_ms); /* libpulse is done: no sound, but the loop goes on */
    pa_mainloop_poll(a->ml);
    pa_mainloop_dispatch(a->ml); /* even after a failed poll(): libpulse wants the pair */
    errno = a->rc_errno;
    return a->rc;
}
