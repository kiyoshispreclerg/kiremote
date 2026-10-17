/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cert.h"
#include "conn.h"
#include "i18n.h"
#include "net.h"
#include "util.h"

/* Keep this much queued for the socket while uploading. */
#define UPLOAD_WINDOW (256u << 10)

struct kr_transfer {
    bool upload;
    SSL_CTX *ctx;
    X509 *peer_cert;
    int listen_fd; /* upload, until someone connects */
    kr_conn *conn;
    int fd;        /* file */
    int64_t size, done;
    bool sent_all; /* upload: waiting for the peer to close */
    double deadline;
    int64_t mtime_ms;
    char name[KR_PATH_MAX]; /* upload: shown name; download: final path */
    char part[KR_PATH_MAX]; /* download: the .part file */
    char why[256];
};

static kr_transfer *transfer_new(SSL_CTX *ctx, X509 *peer_cert, double now)
{
    kr_transfer *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    if (X509_up_ref(peer_cert) != 1) {
        free(t);
        return NULL;
    }
    t->ctx = ctx;
    t->peer_cert = peer_cert;
    t->listen_fd = t->fd = -1;
    t->deadline = now + KR_TRANSFER_TIMEOUT_S;
    return t;
}

kr_transfer *kr_transfer_upload(SSL_CTX *ctx, X509 *peer_cert, int fd, int64_t size,
                                const char *name, double now, int *port)
{
    kr_transfer *t = transfer_new(ctx, peer_cert, now);
    if (!t) {
        close(fd);
        return NULL;
    }
    t->upload = true;
    t->fd = fd;
    t->size = size;
    snprintf(t->name, sizeof t->name, "%s", name);
    t->listen_fd = kr_tcp_listen(KR_PAYLOAD_FIRST, KR_PAYLOAD_LAST, port);
    if (t->listen_fd < 0) {
        kr_transfer_free(t);
        return NULL;
    }
    return t;
}

kr_transfer *kr_transfer_download(SSL_CTX *ctx, X509 *peer_cert, const struct sockaddr_in *addr,
                                  int64_t size, const char *path, int64_t mtime_ms, double now)
{
    kr_transfer *t = transfer_new(ctx, peer_cert, now);
    if (!t)
        return NULL;
    t->size = size;
    t->mtime_ms = mtime_ms;
    snprintf(t->name, sizeof t->name, "%s", path);
    if (!kr_pathf(t->part, sizeof t->part, "%s.part", path)) {
        kr_transfer_free(t);
        errno = ENAMETOOLONG;
        return NULL;
    }
    kr_make_parent_dirs(path);
    /* A leftover .part from an earlier failure gets replaced; O_NOFOLLOW so
     * a planted symlink can't redirect the write. */
    t->fd = open(t->part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (t->fd < 0 || !(t->conn = kr_conn_connect(addr))) {
        int e = errno;
        kr_transfer_free(t);
        errno = e;
        return NULL;
    }
    return t;
}

void kr_transfer_free(kr_transfer *t)
{
    if (!t)
        return;
    if (t->listen_fd >= 0)
        close(t->listen_fd);
    kr_conn_free(t->conn);
    if (t->fd >= 0)
        close(t->fd);
    if (!t->upload && t->part[0])
        unlink(t->part); /* only still there if it never completed */
    X509_free(t->peer_cert);
    free(t);
}

bool kr_transfer_pollfd(const kr_transfer *t, struct pollfd *p)
{
    if (t->listen_fd >= 0) {
        *p = (struct pollfd){.fd = t->listen_fd, .events = POLLIN};
        return true;
    }
    return t->conn && kr_conn_pollfd(t->conn, p);
}

double kr_transfer_deadline(const kr_transfer *t)
{
    return t->deadline;
}

const char *kr_transfer_name(const kr_transfer *t)
{
    return t->name;
}

bool kr_transfer_is_upload(const kr_transfer *t)
{
    return t->upload;
}

bool kr_unique_path(const char *path, int n, char *out, size_t size)
{
    if (n == 0)
        return kr_pathf(out, size, "%s", path);
    /* Before the extension, unless the dot starts the name. */
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base)
        return kr_pathf(out, size, "%s (%d)", path, n);
    return kr_pathf(out, size, "%.*s (%d)%s", (int)(dot - path), path, n, dot);
}

static int failed(kr_transfer *t, const char *why, const char **out)
{
    snprintf(t->why, sizeof t->why, "%s", why);
    if (t->conn)
        kr_conn_close(t->conn);
    *out = t->why;
    return KR_TRANSFER_FAILED;
}

/* The download is complete: give the file its final, unique name. */
static int finish_download(kr_transfer *t, const char **why)
{
    if (t->mtime_ms > 0) {
        struct timespec ts[2] = {{.tv_nsec = UTIME_OMIT},
                                 {.tv_sec = t->mtime_ms / 1000,
                                  .tv_nsec = (t->mtime_ms % 1000) * 1000000}};
        futimens(t->fd, ts);
    }
    if (close(t->fd) != 0) {
        t->fd = -1;
        return failed(t, strerror(errno), why);
    }
    t->fd = -1;
    char final[KR_PATH_MAX];
    for (int n = 0; n < 1000; n++) {
        if (!kr_unique_path(t->name, n, final, sizeof final))
            break;
        /* link() fails if the name exists: no race with other writers. */
        if (link(t->part, final) == 0) {
            unlink(t->part);
            t->part[0] = '\0';
            snprintf(t->name, sizeof t->name, "%s", final);
            return KR_TRANSFER_DONE;
        }
        if (errno != EEXIST)
            return failed(t, strerror(errno), why);
    }
    return failed(t, _("no free file name"), why);
}

static bool peer_ok(kr_transfer *t)
{
    return kr_x509_equal(kr_conn_peer_cert(t->conn), t->peer_cert);
}

static int pump_download(kr_transfer *t, int ev, const char *cwhy, double now, const char **why)
{
    if ((ev & KR_CONN_EV_CONNECTED) && !kr_conn_start_tls(t->conn, t->ctx, false))
        return failed(t, _("TLS handshake failed"), why);
    if ((ev & KR_CONN_EV_SECURE) && !peer_ok(t))
        return failed(t, _("the certificate differs from the one paired"), why);
    size_t len;
    const char *data = kr_conn_data(t->conn, &len);
    if (len) {
        if (t->size >= 0 && (int64_t)len > t->size - t->done)
            len = (size_t)(t->size - t->done);
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(t->fd, data + off, len - off);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return failed(t, strerror(errno), why);
            }
            off += (size_t)w;
        }
        kr_conn_consume(t->conn, len);
        t->done += (int64_t)len;
        t->deadline = now + KR_TRANSFER_TIMEOUT_S;
    }
    if (t->size >= 0 && t->done >= t->size) {
        kr_conn_close(t->conn);
        return finish_download(t, why);
    }
    if (ev & KR_CONN_EV_CLOSED) {
        if (t->size < 0)
            return finish_download(t, why);
        return failed(t, cwhy ? cwhy : _("connection closed"), why);
    }
    return KR_TRANSFER_RUNNING;
}

static int pump_upload(kr_transfer *t, int ev, const char *cwhy, double now, const char **why)
{
    if (ev & KR_CONN_EV_SECURE) {
        if (!peer_ok(t))
            return failed(t, _("the certificate differs from the one paired"), why);
        t->deadline = now + KR_TRANSFER_TIMEOUT_S;
    }
    if (kr_conn_get_state(t->conn) == KR_CONN_OPEN && !t->sent_all) {
        size_t before = kr_conn_pending(t->conn);
        bool wrote = false;
        while (kr_conn_pending(t->conn) < UPLOAD_WINDOW && t->done < t->size) {
            char buf[65536];
            size_t want = sizeof buf;
            if ((int64_t)want > t->size - t->done)
                want = (size_t)(t->size - t->done);
            ssize_t n = read(t->fd, buf, want);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return failed(t, n < 0 ? strerror(errno) : _("the file got shorter"), why);
            if (!kr_conn_write(t->conn, buf, (size_t)n))
                break; /* the pump below reports it */
            t->done += n;
            wrote = true;
        }
        if (t->done >= t->size)
            t->sent_all = true;
        if (wrote || kr_conn_pending(t->conn) < before)
            t->deadline = now + KR_TRANSFER_TIMEOUT_S;
    }
    if (ev & KR_CONN_EV_CLOSED) {
        /* The receiver hangs up once it has everything. */
        if (t->sent_all && kr_conn_pending(t->conn) == 0)
            return KR_TRANSFER_DONE;
        return failed(t, cwhy ? cwhy : _("connection closed"), why);
    }
    return KR_TRANSFER_RUNNING;
}

int kr_transfer_pump(kr_transfer *t, short revents, double now, const char **why)
{
    if (t->upload && t->listen_fd >= 0) {
        if (revents & POLLIN) {
            int fd = accept(t->listen_fd, NULL, NULL);
            if (fd >= 0) {
                close(t->listen_fd);
                t->listen_fd = -1;
                t->conn = kr_conn_wrap(fd);
                /* We listened, so we are the TLS server. */
                if (!t->conn || !kr_conn_start_tls(t->conn, t->ctx, true))
                    return failed(t, _("TLS handshake failed"), why);
                t->deadline = now + KR_TRANSFER_TIMEOUT_S;
                revents = 0;
            }
        }
        if (t->listen_fd >= 0) {
            if (now >= t->deadline)
                return failed(t, _("the other side never fetched the file"), why);
            return KR_TRANSFER_RUNNING;
        }
    }

    const char *cwhy = NULL;
    int ev = kr_conn_pump(t->conn, revents, &cwhy);
    int r = t->upload ? pump_upload(t, ev, cwhy, now, why) : pump_download(t, ev, cwhy, now, why);
    if (r == KR_TRANSFER_RUNNING && now >= t->deadline)
        return failed(t, _("the transfer stalled"), why);
    return r;
}
