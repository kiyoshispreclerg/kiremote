/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CONN_H
#define KR_CONN_H

/* One TCP connection that never blocks, for links and payloads. It starts
 * in plain text (KDE Connect sends one identity line before TLS) and can
 * then switch to TLS in either role: OpenSSL runs on memory BIOs and the
 * bytes move with recv/send, so the socket sits in the daemon's poll()
 * like any other.
 *
 * Bytes received (plain or decrypted) pile up in an input buffer that the
 * owner reads with kr_conn_data/kr_conn_consume or kr_conn_line. The owner
 * keeps the policy: timeouts, what to verify in the peer's certificate,
 * when to give up. */

#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#include <openssl/ssl.h>

typedef struct kr_conn kr_conn;

typedef enum {
    KR_CONN_CONNECTING, /* waiting for connect() */
    KR_CONN_PLAIN,
    KR_CONN_HANDSHAKE,
    KR_CONN_OPEN, /* TLS up */
    KR_CONN_CLOSED,
} kr_conn_state;

/* What kr_conn_pump saw happen. */
enum {
    KR_CONN_EV_CONNECTED = 1 << 0, /* connect() finished */
    KR_CONN_EV_SECURE = 1 << 1,    /* TLS handshake done */
    KR_CONN_EV_DATA = 1 << 2,      /* new input */
    KR_CONN_EV_CLOSED = 1 << 3,    /* over (the input that came first stays) */
};

/* Starts a non-blocking connect; NULL if it failed at once (errno). */
kr_conn *kr_conn_connect(const struct sockaddr_in *to);
/* Takes over an accepted (or socketpair) socket, in plain text. */
kr_conn *kr_conn_wrap(int fd);
void kr_conn_free(kr_conn *c);

kr_conn_state kr_conn_get_state(const kr_conn *c);
/* The peer's IPv4 address (zeroed if unknown). */
const struct sockaddr_in *kr_conn_peer(const kr_conn *c);

/* Switches to TLS. Input already received but not consumed is handed to
 * TLS as the first bytes from the peer. The context holds our certificate
 * and the verify policy; ctx must outlive the connection. */
bool kr_conn_start_tls(kr_conn *c, SSL_CTX *ctx, bool server);
/* The peer's certificate once secure (borrowed), or NULL. */
X509 *kr_conn_peer_cert(const kr_conn *c);

/* Queues bytes: as they are in plain text, encrypted once secure. False
 * if the connection is closed or the queue overflows (then it closes;
 * the next pump says why). */
bool kr_conn_write(kr_conn *c, const void *data, size_t len);
/* Done sending: queues the TLS close_notify and, once everything queued
 * has left, closes our side for writing (FIN). Reading goes on, so the
 * peer's last bytes and its own close are still seen. Android reads a
 * payload until the sender closes, so this is how an upload ends. */
void kr_conn_finish(kr_conn *c);
/* Bytes still waiting to leave (encrypted). */
size_t kr_conn_pending(const kr_conn *c);

/* The pollfd to watch; false once closed. */
bool kr_conn_pollfd(const kr_conn *c, struct pollfd *p);
/* After the poll, with the revents of that pollfd (0 to just flush).
 * Returns KR_CONN_EV_* bits; on KR_CONN_EV_CLOSED, *why (if not NULL)
 * says why, valid until the next call. */
int kr_conn_pump(kr_conn *c, short revents, const char **why);
/* Closes now (what was queued is dropped). */
void kr_conn_close(kr_conn *c);

/* Input not consumed yet. */
const char *kr_conn_data(const kr_conn *c, size_t *len);
void kr_conn_consume(kr_conn *c, size_t n);
/* The next complete line (the "\n" replaced by NUL), consuming it; NULL if
 * none yet. The pointer stays valid until the next call into c. */
char *kr_conn_line(kr_conn *c, size_t *len);

/* SSL_CTX for KDE Connect: our key and certificate, TLS 1.2 or newer,
 * and the peer's certificate always asked for but never judged by
 * OpenSSL (self-signed): the owner checks it after the handshake. */
SSL_CTX *kr_conn_ssl_ctx(X509 *cert, EVP_PKEY *key);

#endif
