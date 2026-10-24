/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "util.h"

#define MAX_CLIENTS 4
#define MAX_LINE 32
#define KEY_MAX 48
#define ASK_TIMEOUT_MS 500

typedef struct {
    int fd;
    char buf[MAX_LINE + 1];
    size_t len;
    double since;
} client;

struct kr_inst {
    int fd;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char mode[16];
    kr_inst_cbs cbs;
    void *ud;
    client clients[MAX_CLIENTS];
};

/* The key as a file name: a serial may have anything in it. */
static bool sock_path(const char *key, char *out, size_t size)
{
    char dir[KR_PATH_MAX], clean[KEY_MAX + 1];
    size_t n = 0;
    for (; key[n] && n < KEY_MAX; n++) {
        unsigned char c = (unsigned char)key[n];
        clean[n] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
                           c == '-' || c == '_'
                       ? (char)c
                       : '_';
    }
    clean[n] = 0;
    if (!n || !kr_runtime_path(dir, sizeof dir, "kiremote-mirror"))
        return false;
    return kr_pathf(out, size, "%s/%s.sock", dir, clean);
}

static bool make_addr(struct sockaddr_un *sa, const char *path)
{
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa->sun_path)
        return false;
    memcpy(sa->sun_path, path, strlen(path) + 1);
    return true;
}

/* Asks the process at path one thing; the answer line into out. */
static bool ask(const char *path, const char *cmd, char *out, size_t size)
{
    struct sockaddr_un sa;
    if (!make_addr(&sa, path))
        return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return false;
    bool ok = false;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0 || errno == EINPROGRESS) {
        char line[MAX_LINE];
        int n = snprintf(line, sizeof line, "%s\n", cmd);
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (n > 0 && poll(&p, 1, ASK_TIMEOUT_MS) == 1 && write(fd, line, (size_t)n) == n) {
            size_t len = 0;
            double end = kr_now() + ASK_TIMEOUT_MS / 1000.0;
            while (len + 1 < size && kr_now() < end) {
                p = (struct pollfd){.fd = fd, .events = POLLIN};
                if (poll(&p, 1, ASK_TIMEOUT_MS) != 1)
                    break;
                ssize_t r = read(fd, out + len, size - 1 - len);
                if (r <= 0)
                    break;
                len += (size_t)r;
                if (memchr(out, '\n', len))
                    break;
            }
            out[len] = 0;
            char *nl = strchr(out, '\n');
            if (nl) {
                *nl = 0;
                ok = true;
            }
        }
    }
    close(fd);
    return ok;
}

kr_inst *kr_inst_claim(const char *key, const char *mode, const kr_inst_cbs *cbs, void *ud, char *other,
                       size_t other_size)
{
    if (other_size)
        other[0] = 0;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (!sock_path(key, path, sizeof path))
        return NULL;
    char dir[sizeof path];
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    if (mkdir(dir, 0700) < 0 && errno != EEXIST)
        return NULL;

    struct sockaddr_un sa;
    if (!make_addr(&sa, path))
        return NULL;
    kr_inst *i = calloc(1, sizeof *i);
    if (!i)
        return NULL;
    i->cbs = *cbs;
    i->ud = ud;
    snprintf(i->mode, sizeof i->mode, "%s", mode);
    for (int attempt = 0; attempt < 2; attempt++) {
        i->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (i->fd < 0)
            break;
        if (bind(i->fd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(i->fd, MAX_CLIENTS) == 0) {
            snprintf(i->path, sizeof i->path, "%s", path);
            chmod(path, 0600);
            return i;
        }
        close(i->fd);
        i->fd = -1;
        if (errno != EADDRINUSE)
            break;
        char their[sizeof i->mode];
        if (ask(path, "status", their, sizeof their)) { /* alive */
            snprintf(other, other_size, "%s", their);
            break;
        }
        unlink(path); /* left by one that died */
    }
    free(i);
    return NULL;
}

void kr_inst_free(kr_inst *i)
{
    if (!i)
        return;
    for (int c = 0; c < MAX_CLIENTS; c++)
        if (i->clients[c].fd > 0)
            close(i->clients[c].fd);
    if (i->fd >= 0)
        close(i->fd);
    if (i->path[0])
        unlink(i->path);
    free(i);
}

int kr_inst_pollfds(const kr_inst *i, struct pollfd *fds, int max)
{
    int n = 0;
    if (max > 0)
        fds[n++] = (struct pollfd){.fd = i->fd, .events = POLLIN};
    for (int c = 0; c < MAX_CLIENTS && n < max; c++)
        if (i->clients[c].fd > 0)
            fds[n++] = (struct pollfd){.fd = i->clients[c].fd, .events = POLLIN};
    return n;
}

static void drop(client *c)
{
    close(c->fd);
    c->fd = 0;
    c->len = 0;
}

static void answer(kr_inst *i, client *c)
{
    char reply[32];
    if (!strcmp(c->buf, "status")) {
        snprintf(reply, sizeof reply, "%s\n", i->mode);
    } else if (!strcmp(c->buf, "raise") || !strcmp(c->buf, "quit")) {
        snprintf(reply, sizeof reply, "ok\n");
        if (c->buf[0] == 'r' && i->cbs.on_raise)
            i->cbs.on_raise(i->ud);
        else if (c->buf[0] == 'q' && i->cbs.on_quit)
            i->cbs.on_quit(i->ud);
    } else {
        snprintf(reply, sizeof reply, "?\n");
    }
    if (write(c->fd, reply, strlen(reply)) < 0) {
        /* the client left: nothing to tell */
    }
    drop(c);
}

void kr_inst_dispatch(kr_inst *i, const struct pollfd *fds, int n)
{
    (void)fds;
    (void)n;
    for (;;) { /* new clients */
        int fd = accept(i->fd, NULL, NULL);
        if (fd < 0)
            break;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        /* A free slot, else the one that has waited longest (it may be silent for good). */
        int slot = 0;
        for (int c = 0; c < MAX_CLIENTS; c++) {
            if (i->clients[c].fd <= 0) {
                slot = c;
                break;
            }
            if (i->clients[c].since < i->clients[slot].since)
                slot = c;
        }
        if (i->clients[slot].fd > 0)
            drop(&i->clients[slot]);
        i->clients[slot] = (client){.fd = fd, .since = kr_now()};
    }
    for (int c = 0; c < MAX_CLIENTS; c++) {
        client *cl = &i->clients[c];
        if (cl->fd <= 0)
            continue;
        ssize_t r = read(cl->fd, cl->buf + cl->len, MAX_LINE - cl->len);
        if (r > 0)
            cl->len += (size_t)r;
        else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
            drop(cl);
            continue;
        }
        cl->buf[cl->len] = 0;
        char *nl = memchr(cl->buf, '\n', cl->len);
        if (nl) {
            *nl = 0;
            answer(i, cl);
        } else if (cl->len >= MAX_LINE) {
            drop(cl);
        }
    }
}

bool kr_inst_query(const char *key, char *mode, size_t size)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    return sock_path(key, path, sizeof path) && ask(path, "status", mode, size) && mode[0] && mode[0] != '?';
}

bool kr_inst_send(const char *key, const char *cmd)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)], reply[16];
    return sock_path(key, path, sizeof path) && ask(path, cmd, reply, sizeof reply) && !strcmp(reply, "ok");
}
