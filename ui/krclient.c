/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "krclient.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "control.h"
#include "i18n.h"
#include "util.h"

#define MAX_IN (4u << 20)
#define MAX_OUT (1u << 20)
#define RETRY_FIRST_S 1.0
#define RETRY_MAX_S 5.0

typedef enum { Q_USER, Q_SUBSCRIBE, Q_STATUS } qkind;

typedef struct {
    qkind kind;
    kr_reply_fn cb;
    void *ud;
} waiting;

struct kr_client {
    char path[KR_PATH_MAX];
    kr_client_ops ops;
    void *ud;
    kr_model *model;
    int fd;
    char *in;
    size_t in_len, in_cap;
    char *out;
    size_t out_len, out_cap;
    waiting *q; /* the requests sent, in order; the replies come in order */
    int n_q, cap_q;
    double retry_at;
    int failures;
};

const kr_model *kr_client_model(const kr_client *c)
{
    return c->model;
}

const char *kr_reply_error(const cJSON *reply)
{
    if (!cJSON_IsObject(reply))
        return _("the daemon is not running");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")))
        return NULL;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(reply, "error");
    return cJSON_IsString(e) ? e->valuestring : _("unknown error");
}

static void change(kr_client *c, kr_change what, const char *id)
{
    if (c->ops.on_change)
        c->ops.on_change(c->ud, what, id);
}

static bool queue_push(kr_client *c, waiting w)
{
    if (c->n_q == c->cap_q) {
        int cap = c->cap_q ? c->cap_q * 2 : 8;
        waiting *nq = realloc(c->q, (size_t)cap * sizeof *nq);
        if (!nq)
            return false;
        c->q = nq;
        c->cap_q = cap;
    }
    c->q[c->n_q++] = w;
    return true;
}

static bool out_append(kr_client *c, const char *s)
{
    size_t n = strlen(s);
    if (c->out_len + n + 1 > MAX_OUT)
        return false;
    if (c->out_len + n + 1 > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap * 2 : 4096;
        while (cap < c->out_len + n + 1)
            cap *= 2;
        char *nb = realloc(c->out, cap);
        if (!nb)
            return false;
        c->out = nb;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, s, n);
    c->out[c->out_len + n] = '\n';
    c->out_len += n + 1;
    return true;
}

static void flush(kr_client *c);

static bool send_raw(kr_client *c, cJSON *req, waiting w)
{
    char *line = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!line)
        return false;
    bool ok = out_append(c, line) && queue_push(c, w);
    free(line);
    if (ok)
        flush(c);
    return ok;
}

static cJSON *simple(const char *type)
{
    cJSON *r = cJSON_CreateObject();
    if (r)
        cJSON_AddStringToObject(r, "type", type);
    return r;
}

/* ---- connection ----------------------------------------------------------- */

static void disconnect(kr_client *c)
{
    bool was_up = c->fd >= 0;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->in_len = c->out_len = 0;

    /* Whoever waits for a reply is told there won't be one. A callback may
     * send again (it will fail: no connection), so take the list first. */
    waiting *q = c->q;
    int n = c->n_q;
    c->q = NULL;
    c->n_q = c->cap_q = 0;
    for (int i = 0; i < n; i++)
        if (q[i].kind == Q_USER && q[i].cb)
            q[i].cb(q[i].ud, NULL);
    free(q);

    bool changed = kr_model_reset(c->model);
    c->failures = was_up ? 0 : c->failures + 1;
    double wait = RETRY_FIRST_S * (c->failures + 1);
    c->retry_at = kr_now() + (wait > RETRY_MAX_S ? RETRY_MAX_S : wait);
    if (changed)
        change(c, KR_CH_DAEMON, NULL);
}

static void try_connect(kr_client *c)
{
    int fd = kr_control_connect(c->path);
    if (fd < 0) {
        c->failures++;
        double wait = RETRY_FIRST_S * (c->failures + 1);
        c->retry_at = kr_now() + (wait > RETRY_MAX_S ? RETRY_MAX_S : wait);
        return;
    }
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(fd);
        c->failures++;
        c->retry_at = kr_now() + RETRY_MAX_S;
        return;
    }
    c->fd = fd;
    c->retry_at = 0;
    c->failures = 0;
    /* Subscribe first, then ask for the state: what happens in between is
     * an event we apply, and the snapshot that follows is at least as new. */
    if (!send_raw(c, simple("subscribe"), (waiting){Q_SUBSCRIBE, NULL, NULL}) ||
        !send_raw(c, simple("status"), (waiting){Q_STATUS, NULL, NULL}))
        disconnect(c);
}

kr_client *kr_client_new(const char *path, const kr_client_ops *ops, void *ud)
{
    kr_client *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->fd = -1;
    c->ud = ud;
    if (ops)
        c->ops = *ops;
    c->model = kr_model_new();
    if (!c->model || !(path ? kr_pathf(c->path, sizeof c->path, "%s", path) : kr_control_default_path(c->path, sizeof c->path))) {
        kr_model_free(c->model);
        free(c);
        return NULL;
    }
    try_connect(c);
    return c;
}

void kr_client_free(kr_client *c)
{
    if (!c)
        return;
    if (c->fd >= 0)
        close(c->fd);
    for (int i = 0; i < c->n_q; i++)
        if (c->q[i].kind == Q_USER && c->q[i].cb)
            c->q[i].cb(c->q[i].ud, NULL);
    kr_model_free(c->model);
    free(c->in);
    free(c->out);
    free(c->q);
    free(c);
}

bool kr_client_send(kr_client *c, cJSON *req, kr_reply_fn cb, void *ud)
{
    if (c->fd < 0 || !req) {
        cJSON_Delete(req);
        if (cb)
            cb(ud, NULL);
        return false;
    }
    if (!send_raw(c, req, (waiting){Q_USER, cb, ud})) {
        if (cb)
            cb(ud, NULL);
        return false;
    }
    return true;
}

/* ---- I/O ------------------------------------------------------------------- */

static void flush(kr_client *c)
{
    while (c->fd >= 0 && c->out_len) {
        ssize_t r = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                disconnect(c);
            return;
        }
        c->out_len -= (size_t)r;
        memmove(c->out, c->out + r, c->out_len);
    }
}

static void handle_line(kr_client *c, const char *line)
{
    cJSON *msg = cJSON_Parse(line);
    if (!cJSON_IsObject(msg)) {
        cJSON_Delete(msg);
        return;
    }
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(msg, "event"))) {
        const char *id;
        switch (kr_model_apply_event(c->model, msg, &id)) {
        case KR_EV_DEVICES:
            change(c, KR_CH_DEVICES, id);
            break;
        case KR_EV_PAIR_REQUEST:
            change(c, KR_CH_PAIR_REQUEST, id);
            break;
        case KR_EV_CONFIG:
            change(c, KR_CH_CONFIG, id);
            break;
        case KR_EV_NONE:
            break;
        }
        cJSON_Delete(msg);
        return;
    }

    /* A reply: it belongs to the oldest request. */
    if (!c->n_q) {
        cJSON_Delete(msg);
        return;
    }
    waiting w = c->q[0];
    memmove(c->q, c->q + 1, (size_t)(--c->n_q) * sizeof *c->q);
    if (w.kind == Q_STATUS) {
        bool was_up = kr_model_daemon_up(c->model);
        if (kr_model_apply_status(c->model, msg)) {
            if (!was_up)
                change(c, KR_CH_DAEMON, NULL);
            change(c, KR_CH_DEVICES, NULL);
        }
    } else if (w.kind == Q_USER && w.cb) {
        w.cb(w.ud, msg);
    }
    cJSON_Delete(msg);
}

static void read_input(kr_client *c)
{
    for (;;) {
        if (c->in_cap - c->in_len < 4096) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 8192;
            if (cap > MAX_IN) {
                disconnect(c);
                return;
            }
            char *nb = realloc(c->in, cap);
            if (!nb) {
                disconnect(c);
                return;
            }
            c->in = nb;
            c->in_cap = cap;
        }
        ssize_t r = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len - 1, 0);
        if (r == 0) {
            disconnect(c);
            return;
        }
        if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                disconnect(c);
            break;
        }
        c->in_len += (size_t)r;
        c->in[c->in_len] = '\0';

        /* Handle each whole line. A callback may drop the connection. */
        size_t start = 0;
        char *nl;
        while (c->fd >= 0 && (nl = memchr(c->in + start, '\n', c->in_len - start))) {
            *nl = '\0';
            handle_line(c, c->in + start);
            start = (size_t)(nl - c->in) + 1;
        }
        if (c->fd < 0)
            return;
        c->in_len -= start;
        memmove(c->in, c->in + start, c->in_len);
    }
}

int kr_client_fd(const kr_client *c)
{
    return c->fd;
}

short kr_client_events(const kr_client *c)
{
    return (short)(POLLIN | (c->out_len ? POLLOUT : 0));
}

void kr_client_dispatch(kr_client *c, short revents)
{
    if (c->fd < 0)
        return;
    if (revents & POLLOUT)
        flush(c);
    if (c->fd >= 0 && (revents & (POLLIN | POLLHUP | POLLERR)))
        read_input(c);
}

double kr_client_deadline(const kr_client *c)
{
    return c->fd < 0 ? c->retry_at : 0;
}

void kr_client_tick(kr_client *c, double now)
{
    if (c->fd < 0 && now >= c->retry_at)
        try_connect(c);
}
