/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The control socket: replies, event subscriptions, clients that don't read. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../harness.h"
#include "control.h"
#include "util.h"

static int big_reply_size;

static void on_request(void *ud, const char *type, const cJSON *req, cJSON *reply)
{
    (void)ud;
    (void)req;
    if (strcmp(type, "echo") == 0) {
        cJSON_AddStringToObject(reply, "said", "hi");
    } else if (strcmp(type, "big") == 0) {
        char *s = malloc((size_t)big_reply_size + 1);
        if (s) {
            memset(s, 'x', (size_t)big_reply_size);
            s[big_reply_size] = '\0';
            cJSON_AddStringToObject(reply, "data", s);
            free(s);
        }
    } else {
        kr_control_fail(reply, "unknown request");
    }
}

typedef struct {
    kr_control *c;
    char dir[64];
    char path[96];
} rig;

static bool rig_open(rig *r)
{
    snprintf(r->dir, sizeof r->dir, "/tmp/krctl.XXXXXX");
    if (!mkdtemp(r->dir))
        return false;
    snprintf(r->path, sizeof r->path, "%s/s.sock", r->dir);
    r->c = kr_control_open(r->path, on_request, NULL);
    return r->c != NULL;
}

static void rig_close(rig *r)
{
    kr_control_close(r->c);
    unlink(r->path);
    rmdir(r->dir);
}

static void pump(kr_control *c)
{
    struct pollfd f[40];
    int n = kr_control_pollfds(c, f, 40);
    poll(f, (nfds_t)n, 20);
    kr_control_dispatch(c, f, n);
}

static int client(rig *r)
{
    int fd = kr_control_connect(r->path);
    if (fd >= 0)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* What was read from a client beyond its last returned line. */
typedef struct {
    int fd;
    char buf[4096];
    size_t len;
} leftover;
static leftover pending[4];

static leftover *leftover_for(int fd)
{
    leftover *spare = &pending[0];
    for (int i = 0; i < 4; i++) {
        if (pending[i].fd == fd)
            return &pending[i];
        if (pending[i].fd == 0)
            spare = &pending[i];
    }
    spare->fd = fd;
    spare->len = 0;
    return spare;
}

/* Reads one line from fd while serving (into buf); NULL if none within
 * ~2 s or if the server closed it. */
static char *read_line(rig *r, int fd, char *buf, size_t size)
{
    leftover *lo = leftover_for(fd);
    for (int i = 0; i < 100; i++) {
        char *nl = memchr(lo->buf, '\n', lo->len);
        if (nl) {
            size_t n = (size_t)(nl - lo->buf);
            if (n >= size)
                return NULL;
            memcpy(buf, lo->buf, n);
            buf[n] = '\0';
            memmove(lo->buf, nl + 1, lo->len - n - 1);
            lo->len -= n + 1;
            return buf;
        }
        pump(r->c);
        ssize_t n = recv(fd, lo->buf + lo->len, sizeof lo->buf - lo->len, 0);
        if (n > 0)
            lo->len += (size_t)n;
        else if (n == 0)
            return NULL;
    }
    return NULL;
}

static void say(int fd, const char *line)
{
    (void)!write(fd, line, strlen(line));
    (void)!write(fd, "\n", 1);
}

TEST(subscribe_without_a_client_is_harmless)
{
    kr_control *c = kr_control_open(NULL, on_request, NULL);
    char *r = kr_control_handle_line(c, "subscribe");
    CHECK_STR_HAS(r, "\"ok\":true");
    free(r);
    CHECK(!kr_control_has_subscribers(c));
    kr_control_close(c);
}

TEST(events_reach_only_subscribers)
{
    rig r = {0};
    CHECK(rig_open(&r));
    int sub = client(&r), other = client(&r);
    CHECK(sub >= 0 && other >= 0);
    char buf[512];

    say(sub, "{\"type\":\"subscribe\"}");
    CHECK(read_line(&r, sub, buf, sizeof buf));
    CHECK_STR_HAS(buf, "\"ok\":true");
    say(other, "echo");
    CHECK(read_line(&r, other, buf, sizeof buf));
    CHECK_STR_HAS(buf, "\"said\":\"hi\"");
    CHECK(kr_control_has_subscribers(r.c));

    kr_control_broadcast(r.c, "{\"event\":\"device\",\"n\":1}");
    kr_control_broadcast(r.c, "{\"event\":\"device\",\"n\":2}");
    CHECK(read_line(&r, sub, buf, sizeof buf));
    CHECK_STR_EQ(buf, "{\"event\":\"device\",\"n\":1}");
    CHECK(read_line(&r, sub, buf, sizeof buf));
    CHECK_STR_EQ(buf, "{\"event\":\"device\",\"n\":2}");

    /* The other one got nothing: its next line is its own reply. */
    say(other, "echo");
    CHECK(read_line(&r, other, buf, sizeof buf));
    CHECK_STR_HAS(buf, "\"said\"");

    /* A subscriber still answers requests, and can stop. */
    say(sub, "echo");
    CHECK(read_line(&r, sub, buf, sizeof buf));
    CHECK_STR_HAS(buf, "\"said\"");
    say(sub, "unsubscribe");
    CHECK(read_line(&r, sub, buf, sizeof buf));
    CHECK(!kr_control_has_subscribers(r.c));

    close(sub);
    close(other);
    rig_close(&r);
}

TEST(closing_the_client_drops_the_subscription)
{
    rig r = {0};
    CHECK(rig_open(&r));
    int sub = client(&r);
    char buf[256];
    say(sub, "subscribe");
    CHECK(read_line(&r, sub, buf, sizeof buf));
    CHECK(kr_control_has_subscribers(r.c));
    close(sub);
    for (int i = 0; i < 5; i++)
        pump(r.c);
    CHECK(!kr_control_has_subscribers(r.c));
    rig_close(&r);
}

/* A reply bigger than the socket buffer arrives whole. */
TEST(big_reply_is_not_cut)
{
    rig r = {0};
    CHECK(rig_open(&r));
    big_reply_size = 1 << 20;
    int fd = client(&r);
    say(fd, "big");

    size_t cap = (size_t)big_reply_size + 256, len = 0;
    char *buf = malloc(cap);
    CHECK(buf);
    bool done = false;
    for (int i = 0; i < 2000 && !done; i++) {
        pump(r.c);
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n > 0) {
            len += (size_t)n;
            buf[len] = '\0';
            done = strchr(buf, '\n') != NULL;
        }
    }
    CHECK(done);
    CHECK_INT_EQ(len, strlen("{\"ok\":true,\"data\":\"") + (size_t)big_reply_size + strlen("\"}") + 1);
    CHECK_STR_HAS(buf, "xxxx\"}\n");
    free(buf);
    close(fd);
    rig_close(&r);
}

/* One that never reads must not make the daemon grow without bound, and
 * must not stop the others. */
TEST(a_subscriber_that_does_not_read_is_dropped)
{
    rig r = {0};
    CHECK(rig_open(&r));
    int slow = client(&r), good = client(&r);
    char buf[512];
    say(slow, "subscribe");
    CHECK(read_line(&r, slow, buf, sizeof buf));
    say(good, "subscribe");
    CHECK(read_line(&r, good, buf, sizeof buf));

    char line[512];
    memset(line, 'y', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    line[0] = '{';
    for (int i = 0; i < 3000; i++) { /* 1.5 MB */
        kr_control_broadcast(r.c, line);
        char drain[4096];
        while (recv(good, drain, sizeof drain, 0) > 0)
            ;
        if (i % 20 == 0)
            pump(r.c);
    }
    for (int i = 0; i < 5; i++)
        pump(r.c);

    /* The slow one was closed by the server: whatever was in flight, then EOF. */
    bool eof = false;
    char drain[4096];
    for (int i = 0; i < 2000 && !eof; i++) {
        ssize_t n = recv(slow, drain, sizeof drain, 0);
        eof = n == 0;
    }
    CHECK(eof);

    /* The good one is still subscribed and served. */
    CHECK(kr_control_has_subscribers(r.c));
    kr_control_broadcast(r.c, "{\"event\":\"after\"}");
    bool saw = false;
    for (int i = 0; i < 400 && !saw; i++) {
        char *l = read_line(&r, good, buf, sizeof buf);
        saw = l && strstr(l, "after");
    }
    CHECK(saw);

    close(slow);
    close(good);
    rig_close(&r);
}

TEST(pollout_only_while_something_waits)
{
    rig r = {0};
    CHECK(rig_open(&r));
    int fd = client(&r);
    char buf[256];
    say(fd, "subscribe");
    CHECK(read_line(&r, fd, buf, sizeof buf));

    struct pollfd f[8];
    int n = kr_control_pollfds(r.c, f, 8);
    for (int i = 0; i < n; i++)
        CHECK(!(f[i].events & POLLOUT));

    char line[1024];
    memset(line, 'z', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    /* Until the socket buffer is full and the rest has to wait (but not so
     * long that the client is dropped for it). */
    bool out = false;
    for (int i = 0; i < 240 && !out; i++) {
        kr_control_broadcast(r.c, line);
        n = kr_control_pollfds(r.c, f, 8);
        for (int k = 0; k < n; k++)
            out |= (f[k].events & POLLOUT) != 0;
    }
    CHECK(out);
    CHECK(kr_control_has_subscribers(r.c));

    close(fd);
    rig_close(&r);
}

int main(void)
{
    RUN(subscribe_without_a_client_is_harmless);
    RUN(events_reach_only_subscribers);
    RUN(closing_the_client_drops_the_subscription);
    RUN(big_reply_is_not_cut);
    RUN(a_subscriber_that_does_not_read_is_dropped);
    RUN(pollout_only_while_something_waits);
    return harness_report();
}
