/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"
#include "util.h"

#define MAX_CLIENTS 16
#define MAX_LINE 65536

typedef struct {
    int fd;
    char *buf; /* MAX_LINE bytes */
    size_t len;
} client;

struct kr_control {
    int fd; /* -1 without a socket (tests) */
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    kr_control_fn on_request;
    void *ud;
    client clients[MAX_CLIENTS];
    int n_clients;
};

static bool set_flags(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static bool make_addr(struct sockaddr_un *sa, const char *path)
{
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    return kr_pathf(sa->sun_path, sizeof sa->sun_path, "%s", path);
}

/* True if something accepts connections at path. */
static bool someone_listens(const struct sockaddr_un *sa)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    bool yes = connect(fd, (const struct sockaddr *)sa, sizeof *sa) == 0;
    close(fd);
    return yes;
}

kr_control *kr_control_open(const char *path, kr_control_fn on_request, void *ud)
{
    kr_control *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->fd = -1;
    c->on_request = on_request;
    c->ud = ud;
    if (!path)
        return c;

    struct sockaddr_un sa;
    if (!make_addr(&sa, path)) {
        kr_log_error(_("socket path too long: %s"), path);
        free(c);
        return NULL;
    }
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            kr_log_error(_("%s exists and is not a socket; leaving it alone"), path);
            free(c);
            return NULL;
        }
        if (someone_listens(&sa)) {
            kr_log_error(_("another kiremoted is already running (%s)"), path);
            free(c);
            return NULL;
        }
        unlink(path); /* left behind by a crash */
    }

    kr_make_parent_dirs(path);
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    mode_t old = umask(0177);
    bool ok = c->fd >= 0 && set_flags(c->fd) && bind(c->fd, (struct sockaddr *)&sa, sizeof sa) == 0;
    umask(old);
    if (!ok || listen(c->fd, 8) < 0) {
        kr_log_error(_("socket %s: %s"), path, strerror(errno));
        if (c->fd >= 0)
            close(c->fd);
        free(c);
        return NULL;
    }
    snprintf(c->path, sizeof c->path, "%s", path);
    return c;
}

static void drop_client(kr_control *c, int i)
{
    close(c->clients[i].fd);
    free(c->clients[i].buf);
    c->clients[i] = c->clients[--c->n_clients];
}

void kr_control_close(kr_control *c)
{
    if (!c)
        return;
    while (c->n_clients)
        drop_client(c, 0);
    if (c->fd >= 0) {
        close(c->fd);
        unlink(c->path);
    }
    free(c);
}

int kr_control_pollfds(const kr_control *c, struct pollfd *fds, int max)
{
    int n = 0;
    if (c->fd >= 0 && n < max)
        fds[n++] = (struct pollfd){.fd = c->fd, .events = POLLIN};
    for (int i = 0; i < c->n_clients && n < max; i++)
        fds[n++] = (struct pollfd){.fd = c->clients[i].fd, .events = POLLIN};
    return n;
}

void kr_control_fail(cJSON *reply, const char *error)
{
    cJSON_ReplaceItemInObjectCaseSensitive(reply, "ok", cJSON_CreateFalse());
    cJSON_DeleteItemFromObjectCaseSensitive(reply, "error");
    cJSON_AddStringToObject(reply, "error", error);
}

char *kr_control_handle_line(kr_control *c, const char *line)
{
    cJSON *reply = cJSON_CreateObject();
    if (!reply)
        return NULL;
    cJSON_AddTrueToObject(reply, "ok");

    while (*line == ' ' || *line == '\t')
        line++;
    cJSON *req = NULL;
    if (*line == '{') {
        req = cJSON_Parse(line);
    } else {
        /* A bare word, as typed in a terminal. */
        char word[32];
        size_t n = strcspn(line, " \t\r");
        if (n && n < sizeof word) {
            memcpy(word, line, n);
            word[n] = '\0';
            req = cJSON_CreateObject();
            if (req)
                cJSON_AddStringToObject(req, "type", word);
        }
    }

    const cJSON *t = req ? cJSON_GetObjectItemCaseSensitive(req, "type") : NULL;
    if (!cJSON_IsObject(req))
        kr_control_fail(reply, _("invalid JSON"));
    else if (!cJSON_IsString(t))
        kr_control_fail(reply, _("missing \"type\""));
    else if (c->on_request)
        c->on_request(c->ud, t->valuestring, req, reply);
    else
        kr_control_fail(reply, _("unknown request"));

    char *out = cJSON_PrintUnformatted(reply);
    cJSON_Delete(req);
    cJSON_Delete(reply);
    return out;
}

static void send_line(int fd, const char *s)
{
    /* Best effort: a full socket buffer means the client isn't reading. */
    size_t n = strlen(s);
    if (send(fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)n)
        send(fd, "\n", 1, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void accept_clients(kr_control *c)
{
    for (;;) {
        int fd = accept(c->fd, NULL, NULL);
        if (fd < 0)
            return;
        if (c->n_clients == MAX_CLIENTS || !set_flags(fd)) {
            close(fd);
            continue;
        }
        char *buf = malloc(MAX_LINE);
        if (!buf) {
            close(fd);
            continue;
        }
        c->clients[c->n_clients++] = (client){.fd = fd, .buf = buf};
    }
}

/* Reads what is there and answers each complete line. False to drop the
 * client (closed, error, or a line too long). */
static bool serve(kr_control *c, client *cl)
{
    for (;;) {
        ssize_t r = recv(cl->fd, cl->buf + cl->len, MAX_LINE - 1 - cl->len, 0);
        if (r == 0)
            return false;
        if (r < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        cl->len += (size_t)r;
        cl->buf[cl->len] = '\0';

        char *start = cl->buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = '\0';
            if (nl > start && nl[-1] == '\r')
                nl[-1] = '\0';
            if (*start) {
                char *reply = kr_control_handle_line(c, start);
                if (reply)
                    send_line(cl->fd, reply);
                free(reply);
            }
            start = nl + 1;
        }
        cl->len -= (size_t)(start - cl->buf);
        memmove(cl->buf, start, cl->len);
        if (cl->len == MAX_LINE - 1)
            return false;
    }
}

void kr_control_dispatch(kr_control *c, const struct pollfd *fds, int n)
{
    for (int k = 0; k < n; k++) {
        if (!fds[k].revents)
            continue;
        if (fds[k].fd == c->fd) {
            accept_clients(c);
            continue;
        }
        for (int i = 0; i < c->n_clients; i++)
            if (c->clients[i].fd == fds[k].fd) {
                if (!serve(c, &c->clients[i]))
                    drop_client(c, i);
                break;
            }
    }
}

bool kr_control_default_path(char *out, size_t size)
{
    return kr_runtime_path(out, size, "kiremote.sock");
}

char *kr_control_request(const char *path, const char *line, int timeout_ms)
{
    struct sockaddr_un sa;
    if (!make_addr(&sa, path)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        send(fd, line, strlen(line), MSG_NOSIGNAL) < 0 || send(fd, "\n", 1, MSG_NOSIGNAL) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    size_t len = 0, cap = 4096;
    char *reply = malloc(cap);
    double deadline = kr_now() + timeout_ms / 1000.0;
    while (reply) {
        int left = (int)((deadline - kr_now()) * 1000);
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (left <= 0 || poll(&p, 1, left) <= 0)
            break;
        if (cap - len < 4096) {
            char *nr = cap < (16u << 20) ? realloc(reply, cap * 2) : NULL;
            if (!nr)
                break;
            reply = nr;
            cap *= 2;
        }
        ssize_t r = recv(fd, reply + len, cap - 1 - len, 0);
        if (r <= 0)
            break;
        len += (size_t)r;
        reply[len] = '\0';
        char *nl = strchr(reply, '\n');
        if (nl) {
            *nl = '\0';
            close(fd);
            return reply;
        }
    }
    free(reply);
    close(fd);
    errno = ETIMEDOUT;
    return NULL;
}
