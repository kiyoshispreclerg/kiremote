/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Grown from kikarinhas' tls.c: same memory-BIO scheme, but on sockets we
 * already have, in both TLS roles, with a plain-text phase first. */
#define _DEFAULT_SOURCE /* TCP_KEEPIDLE and friends */
#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>

#include "i18n.h"

/* Read at most this much per pump, so one busy peer can't starve the loop
 * (the socket stays readable, so poll() comes back for the rest). */
#define MAX_READ_PER_PUMP (1u << 20)
/* Packets go up to 32 MiB; something is very wrong past this. */
#define MAX_OUT (48u << 20)

struct kr_conn {
    int fd;
    kr_conn_state st;
    struct sockaddr_in peer;

    SSL *ssl;
    BIO *rbio, *wbio; /* network → SSL, SSL → network */

    char *out; /* bytes waiting for send() */
    size_t out_len, out_cap;

    char *in; /* input: [in_off, in_len) not consumed yet */
    size_t in_off, in_len, in_cap;
    size_t scan;      /* bytes after in_off already searched for "\n" */
    size_t line_skip; /* the last line kr_conn_line gave, consumed next call */

    bool finishing, wr_shut; /* kr_conn_finish called; FIN sent */
    bool failed; /* closed with an error not reported yet */
    char why[256];
};

static void set_keepalive(int fd)
{
    /* As kdeconnect-kde's configureSocket: notice a vanished peer within
     * about a minute instead of two hours. */
    int on = 1, idle = 30, intvl = 10, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
}

static bool set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static kr_conn *conn_new(int fd, kr_conn_state st)
{
    kr_conn *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->fd = fd;
    c->st = st;
    return c;
}

kr_conn *kr_conn_connect(const struct sockaddr_in *to)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    if (!set_nonblock(fd) ||
        (connect(fd, (const struct sockaddr *)to, sizeof *to) < 0 && errno != EINPROGRESS)) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    kr_conn *c = conn_new(fd, KR_CONN_CONNECTING);
    if (!c) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    c->peer = *to;
    return c;
}

kr_conn *kr_conn_wrap(int fd)
{
    if (!set_nonblock(fd))
        return NULL;
    kr_conn *c = conn_new(fd, KR_CONN_PLAIN);
    if (!c)
        return NULL;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0 && ss.ss_family == AF_INET) {
        memcpy(&c->peer, &ss, sizeof c->peer);
        set_keepalive(fd);
    }
    return c;
}

static void drop(kr_conn *c)
{
    if (c->ssl)
        SSL_free(c->ssl); /* frees the BIOs too */
    c->ssl = NULL;
    c->rbio = c->wbio = NULL;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->out_len = 0;
    c->st = KR_CONN_CLOSED;
}

static void fail(kr_conn *c, const char *why)
{
    if (c->st == KR_CONN_CLOSED)
        return;
    drop(c);
    c->failed = true;
    snprintf(c->why, sizeof c->why, "%s", why);
}

void kr_conn_close(kr_conn *c)
{
    drop(c);
}

void kr_conn_free(kr_conn *c)
{
    if (!c)
        return;
    drop(c);
    free(c->out);
    free(c->in);
    free(c);
}

kr_conn_state kr_conn_get_state(const kr_conn *c)
{
    return c->st;
}

const struct sockaddr_in *kr_conn_peer(const kr_conn *c)
{
    return &c->peer;
}

X509 *kr_conn_peer_cert(const kr_conn *c)
{
    return c->ssl && c->st == KR_CONN_OPEN ? SSL_get0_peer_certificate(c->ssl) : NULL;
}

/* ---- buffers ------------------------------------------------------------- */

static bool grow(char **buf, size_t *cap, size_t need, size_t max)
{
    if (need <= *cap)
        return true;
    if (need > max)
        return false;
    size_t n = *cap ? *cap : 16384;
    while (n < need)
        n *= 2;
    char *b = realloc(*buf, n);
    if (!b)
        return false;
    *buf = b;
    *cap = n;
    return true;
}

/* Room for n more input bytes (plus a NUL for kr_conn_line). */
static char *in_space(kr_conn *c, size_t n)
{
    if (c->in_off && c->in_len + n + 1 > c->in_cap) {
        memmove(c->in, c->in + c->in_off, c->in_len - c->in_off);
        c->in_len -= c->in_off;
        c->in_off = 0;
    }
    if (!grow(&c->in, &c->in_cap, c->in_len + n + 1, SIZE_MAX / 2))
        return NULL;
    return c->in + c->in_len;
}

static bool append_out(kr_conn *c, const void *data, size_t n)
{
    if (!grow(&c->out, &c->out_cap, c->out_len + n, MAX_OUT))
        return false;
    memcpy(c->out + c->out_len, data, n);
    c->out_len += n;
    return true;
}

const char *kr_conn_data(const kr_conn *c, size_t *len)
{
    *len = c->in_len - c->in_off;
    return c->in ? c->in + c->in_off : "";
}

void kr_conn_consume(kr_conn *c, size_t n)
{
    size_t have = c->in_len - c->in_off;
    if (n > have)
        n = have;
    c->in_off += n;
    c->scan = c->scan > n ? c->scan - n : 0;
    if (c->in_off == c->in_len)
        c->in_off = c->in_len = 0;
}

char *kr_conn_line(kr_conn *c, size_t *len)
{
    if (c->line_skip) {
        size_t n = c->line_skip;
        c->line_skip = 0;
        kr_conn_consume(c, n);
    }
    size_t have = c->in_len - c->in_off;
    if (c->scan >= have)
        return NULL;
    char *start = c->in + c->in_off;
    char *nl = memchr(start + c->scan, '\n', have - c->scan);
    if (!nl) {
        c->scan = have;
        return NULL;
    }
    *nl = '\0';
    *len = (size_t)(nl - start);
    c->line_skip = *len + 1;
    c->scan = 0;
    return start;
}

/* ---- I/O ----------------------------------------------------------------- */

/* OpenSSL's reason for the last failure, or fallback. */
static const char *ssl_reason(const char *fallback)
{
    static char buf[256];
    unsigned long e = ERR_get_error();
    ERR_clear_error();
    if (e) {
        ERR_error_string_n(e, buf, sizeof buf);
        return buf;
    }
    return fallback;
}

/* Moves what SSL produced to the out queue and sends what the socket
 * takes. False (and closed) on error. */
static bool flush(kr_conn *c)
{
    if (c->wbio) {
        char buf[16384];
        int n;
        while ((n = BIO_read(c->wbio, buf, sizeof buf)) > 0)
            if (!append_out(c, buf, (size_t)n)) {
                fail(c, _("too much data waiting to be sent"));
                return false;
            }
    }
    size_t sent = 0;
    while (sent < c->out_len) {
        ssize_t w = send(c->fd, c->out + sent, c->out_len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            fail(c, strerror(errno));
            return false;
        }
        sent += (size_t)w;
    }
    if (sent) {
        memmove(c->out, c->out + sent, c->out_len - sent);
        c->out_len -= sent;
    }
    if (c->finishing && !c->wr_shut && c->out_len == 0) {
        shutdown(c->fd, SHUT_WR);
        c->wr_shut = true;
    }
    return true;
}

void kr_conn_finish(kr_conn *c)
{
    if (c->st != KR_CONN_OPEN || c->finishing)
        return;
    c->finishing = true;
    SSL_shutdown(c->ssl); /* queues close_notify; the peer's reply is not awaited */
    flush(c);
}

bool kr_conn_write(kr_conn *c, const void *data, size_t len)
{
    if (c->st == KR_CONN_PLAIN || c->st == KR_CONN_CONNECTING) {
        if (!append_out(c, data, len)) {
            fail(c, _("too much data waiting to be sent"));
            return false;
        }
        return c->st == KR_CONN_CONNECTING || flush(c);
    }
    if (c->st != KR_CONN_OPEN)
        return false;
    const char *p = data;
    while (len) {
        int chunk = len > (1u << 30) ? 1 << 30 : (int)len;
        /* A memory BIO takes everything. */
        if (SSL_write(c->ssl, p, chunk) <= 0) {
            fail(c, ssl_reason(_("TLS write error")));
            return false;
        }
        p += chunk;
        len -= (size_t)chunk;
    }
    return flush(c);
}

size_t kr_conn_pending(const kr_conn *c)
{
    return c->out_len + (c->wbio ? BIO_ctrl_pending(c->wbio) : 0);
}

bool kr_conn_start_tls(kr_conn *c, SSL_CTX *ctx, bool server)
{
    if (c->st != KR_CONN_PLAIN)
        return false;
    c->ssl = SSL_new(ctx);
    c->rbio = BIO_new(BIO_s_mem());
    c->wbio = BIO_new(BIO_s_mem());
    if (!c->ssl || !c->rbio || !c->wbio) {
        BIO_free(c->rbio);
        BIO_free(c->wbio);
        c->rbio = c->wbio = NULL;
        fail(c, _("out of memory"));
        return false;
    }
    SSL_set_bio(c->ssl, c->rbio, c->wbio);
    if (server)
        SSL_set_accept_state(c->ssl);
    else
        SSL_set_connect_state(c->ssl);
    c->st = KR_CONN_HANDSHAKE;

    /* Whatever came after the plain-text line already belongs to TLS
     * (the line itself was consumed by kr_conn_line). */
    if (c->line_skip) {
        kr_conn_consume(c, c->line_skip);
        c->line_skip = 0;
    }
    size_t n;
    const char *rest = kr_conn_data(c, &n);
    if (n)
        BIO_write(c->rbio, rest, (int)n);
    c->in_off = c->in_len = c->scan = c->line_skip = 0;

    /* The client speaks first. */
    if (!server) {
        int r = SSL_do_handshake(c->ssl);
        int e = SSL_get_error(c->ssl, r);
        if (r <= 0 && e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            fail(c, ssl_reason(_("TLS handshake failed")));
            return false;
        }
    }
    return flush(c);
}

/* Runs SSL on what arrived. Returns KR_CONN_EV_* bits; closes on error. */
static int drive(kr_conn *c)
{
    int ev = 0;
    if (c->st == KR_CONN_HANDSHAKE) {
        int r = SSL_do_handshake(c->ssl);
        if (r == 1) {
            c->st = KR_CONN_OPEN;
            ev |= KR_CONN_EV_SECURE;
        } else {
            int e = SSL_get_error(c->ssl, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                fail(c, ssl_reason(_("TLS handshake failed")));
                return ev;
            }
        }
    }
    while (c->st == KR_CONN_OPEN) {
        char *dst = in_space(c, 16384);
        if (!dst) {
            fail(c, _("out of memory"));
            return ev;
        }
        int n = SSL_read(c->ssl, dst, 16384);
        if (n > 0) {
            c->in_len += (size_t)n;
            ev |= KR_CONN_EV_DATA;
            continue;
        }
        int e = SSL_get_error(c->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            break;
        fail(c, e == SSL_ERROR_ZERO_RETURN ? _("connection closed by the other side")
                                           : ssl_reason(_("TLS read error")));
        return ev;
    }
    return ev;
}

/* Reads what the socket has. Returns KR_CONN_EV_DATA (plain text) and
 * sets *eof when the peer closed its side. */
static int read_socket(kr_conn *c, bool *eof)
{
    int ev = 0;
    size_t total = 0;
    char tmp[16384];
    while (total < MAX_READ_PER_PUMP) {
        char *dst = tmp;
        if (c->st == KR_CONN_PLAIN && !(dst = in_space(c, sizeof tmp))) {
            fail(c, _("out of memory"));
            return ev;
        }
        ssize_t n = recv(c->fd, dst, sizeof tmp, MSG_DONTWAIT);
        if (n > 0) {
            total += (size_t)n;
            if (c->st == KR_CONN_PLAIN) {
                c->in_len += (size_t)n;
                ev |= KR_CONN_EV_DATA;
            } else {
                BIO_write(c->rbio, tmp, (int)n);
            }
            continue;
        }
        if (n == 0) {
            *eof = true;
            return ev;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            fail(c, strerror(errno));
        return ev;
    }
    return ev;
}

int kr_conn_pump(kr_conn *c, short revents, const char **why)
{
    int ev = 0;
    if (c->st == KR_CONN_CONNECTING) {
        if (!(revents & (POLLOUT | POLLERR | POLLHUP)))
            return 0;
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        if (err) {
            fail(c, strerror(err));
        } else {
            c->st = KR_CONN_PLAIN;
            set_keepalive(c->fd);
            ev |= KR_CONN_EV_CONNECTED;
        }
    }

    bool eof = false;
    if (c->st != KR_CONN_CLOSED && (revents & (POLLIN | POLLHUP | POLLERR)))
        ev |= read_socket(c, &eof);
    if (c->st == KR_CONN_HANDSHAKE || c->st == KR_CONN_OPEN)
        ev |= drive(c);
    if (c->st != KR_CONN_CLOSED && flush(c) && eof)
        fail(c, _("connection closed by the other side"));

    if (c->st == KR_CONN_CLOSED) {
        ev |= KR_CONN_EV_CLOSED;
        if (why)
            *why = c->failed ? c->why : _("connection closed");
    }
    return ev;
}

bool kr_conn_pollfd(const kr_conn *c, struct pollfd *p)
{
    if (c->st == KR_CONN_CLOSED)
        return false;
    short events = c->st == KR_CONN_CONNECTING ? POLLOUT : POLLIN;
    if (c->out_len)
        events |= POLLOUT;
    *p = (struct pollfd){.fd = c->fd, .events = events};
    return true;
}

/* ---- TLS context --------------------------------------------------------- */

static int accept_any(int ok, X509_STORE_CTX *store)
{
    /* Self-signed certificates everywhere: the owner pins them after the
     * handshake (trusted devices) or takes them as they come (pairing). */
    (void)ok;
    (void)store;
    return 1;
}

SSL_CTX *kr_conn_ssl_ctx(X509 *cert, EVP_PKEY *key)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx)
        return NULL;
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) ||
        SSL_CTX_use_certificate(ctx, cert) != 1 || SSL_CTX_use_PrivateKey(ctx, key) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    /* As a server this asks for the client's certificate. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, accept_any);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(ctx, 0);
    return ctx;
}
