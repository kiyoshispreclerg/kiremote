/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "instance.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "control.h"
#include "util.h"

#define MAX_REQUEST 2048
/* A client that connects and says nothing must not hold a slot for long. */
#define CLIENT_TIMEOUT_S 2

static struct {
    int fd;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    guint watch;
    kr_instance_fn fn;
    void *ud;
} self = {.fd = -1};

typedef struct {
    int fd;
    char buf[MAX_REQUEST + 1];
    size_t len;
    guint watch, timeout;
} client;

char *kr_instance_path(const char *control_socket)
{
    char def[KR_PATH_MAX];
    if (!control_socket) {
        if (!kr_control_default_path(def, sizeof def))
            return NULL;
        control_socket = def;
    }
    size_t n = strlen(control_socket);
    if (n > 5 && strcmp(control_socket + n - 5, ".sock") == 0)
        return g_strdup_printf("%.*s-ui.sock", (int)(n - 5), control_socket);
    return g_strdup_printf("%s-ui", control_socket);
}

static gboolean make_addr(struct sockaddr_un *sa, const char *path)
{
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa->sun_path)
        return FALSE;
    strcpy(sa->sun_path, path); /* length checked above */
    return TRUE;
}

/* ---- the one that listens ------------------------------------------------------ */

static void client_free(client *c)
{
    if (c->watch)
        g_source_remove(c->watch);
    if (c->timeout)
        g_source_remove(c->timeout);
    close(c->fd);
    g_free(c);
}

static gboolean client_timeout(gpointer data)
{
    client *c = data;
    c->timeout = 0;
    client_free(c);
    return G_SOURCE_REMOVE;
}

/* The request is its words, one per line, ended by an empty line. */
static void deliver(client *c)
{
    c->buf[c->len] = '\0';
    char *end = strstr(c->buf, "\n\n");
    if (!end)
        return;
    *end = '\0';
    char **words = g_strsplit(c->buf, "\n", -1);
    if (self.fn)
        self.fn(self.ud, words);
    g_strfreev(words);
}

static gboolean client_readable(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    client *c = data;
    (void)ch;
    gboolean done = FALSE;
    if (cond & G_IO_IN) {
        ssize_t r = recv(c->fd, c->buf + c->len, MAX_REQUEST - c->len, 0);
        if (r > 0) {
            c->len += (size_t)r;
            c->buf[c->len] = '\0';
            if (strstr(c->buf, "\n\n")) {
                deliver(c);
                done = TRUE;
            } else if (c->len == MAX_REQUEST) {
                done = TRUE; /* too long to be ours: drop it unread */
            }
        } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
            done = TRUE;
        }
    } else {
        done = TRUE;
    }
    if (done) {
        c->watch = 0; /* returning FALSE removes it */
        client_free(c);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean accept_ready(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)ch;
    (void)cond;
    (void)data;
    for (;;) {
        int fd = accept(self.fd, NULL, NULL);
        if (fd < 0)
            return G_SOURCE_CONTINUE;
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            close(fd);
            continue;
        }
        client *c = g_new0(client, 1);
        c->fd = fd;
        GIOChannel *ch2 = g_io_channel_unix_new(fd);
        c->watch = g_io_add_watch(ch2, G_IO_IN | G_IO_HUP | G_IO_ERR, client_readable, c);
        g_io_channel_unref(ch2);
        c->timeout = g_timeout_add_seconds(CLIENT_TIMEOUT_S, client_timeout, c);
    }
}

/* ---- the one that comes later ------------------------------------------------------ */

/* True if somebody answered at path, and now has words. */
static gboolean hand_over(const struct sockaddr_un *sa, char **words)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return FALSE;
    if (connect(fd, (const struct sockaddr *)sa, sizeof *sa) < 0) {
        close(fd);
        return FALSE;
    }
    char *joined = g_strjoinv("\n", words);
    char *msg = g_strconcat(joined, "\n\n", NULL);
    g_free(joined);
    size_t n = strlen(msg);
    (void)!send(fd, msg, n, MSG_NOSIGNAL); /* a few words: one send; if not, the instance will not act and the user retries */
    g_free(msg);
    close(fd);
    return TRUE;
}

gboolean kr_instance_acquire(const char *path, char **words, kr_instance_fn on_request, void *ud)
{
    struct sockaddr_un sa;
    if (!path || !make_addr(&sa, path))
        return TRUE;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (hand_over(&sa, words))
            return FALSE;
        /* Nobody there: a leftover file goes, then we listen. */
        unlink(path);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0)
            return TRUE;
        mode_t old = umask(0177);
        int rc = bind(fd, (struct sockaddr *)&sa, sizeof sa);
        umask(old);
        if (rc == 0 && listen(fd, 4) == 0) {
            self.fd = fd;
            snprintf(self.path, sizeof self.path, "%s", path);
            self.fn = on_request;
            self.ud = ud;
            GIOChannel *ch = g_io_channel_unix_new(fd);
            self.watch = g_io_add_watch(ch, G_IO_IN, accept_ready, NULL);
            g_io_channel_unref(ch);
            return TRUE;
        }
        close(fd);
        if (errno != EADDRINUSE)
            return TRUE; /* cannot listen here: carry on as the only one */
        /* Two launches at once: the other one won the bind; talk to it. */
    }
    return TRUE;
}

void kr_instance_release(void)
{
    if (self.fd < 0)
        return;
    g_source_remove(self.watch);
    close(self.fd);
    unlink(self.path);
    self.fd = -1;
    self.watch = 0;
    self.fn = NULL;
}
