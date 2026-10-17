/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_LINK_H
#define KR_LINK_H

/* A link: one TCP+TLS connection to one device, from the handshake to
 * the packets (lanlinkprovider.cpp and landevicelink.cpp upstream).
 *
 * Handshake (docs/PROTOCOL.md, "Descoberta e conexão"):
 *   we connected (kr_link_connect): send our connection identity in plain
 *     text, then TLS as the *server*;
 *   they connected (kr_link_accept): read their connection identity, then
 *     TLS as the *client*;
 *   then both: check the peer's certificate (CN = device id; the pinned
 *   one if trusted), send our full identity, read theirs.
 *
 * The link is ready when the peer's full identity checks out; from then
 * on kr_link_next hands out its packets. The link owns its connection, the
 * peer's identity packet and a reference to its certificate. */

#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>

#include <openssl/ssl.h>

#include "packet.h"
#include "trust.h"

/* What links share, owned by the daemon; must outlive them. */
typedef struct {
    SSL_CTX *ctx;
    const kr_packet *identity; /* our full identity (deviceId, name, ...) */
    const kr_trust *trust;
    double timeout; /* seconds for the whole handshake */
} kr_link_env;

typedef struct kr_link kr_link;

/* What kr_link_pump saw (bits). */
enum {
    KR_LINK_EV_READY = 1 << 0,  /* handshake done, once */
    KR_LINK_EV_CLOSED = 1 << 1, /* over; free it (packets that came first
                                   can still be taken) */
};

/* After a discovery: connects to addr, expecting device id with protocol
 * version proto. NULL if connect() failed at once. */
kr_link *kr_link_connect(const kr_link_env *env, const struct sockaddr_in *addr,
                         const char *id, int proto, double now);
/* A connection someone opened to our TCP port. NULL (fd closed) on error. */
kr_link *kr_link_accept(const kr_link_env *env, int fd, double now);
/* Over a socket that is already connected (socketpair, tests): as
 * kr_link_connect when id is set, else as kr_link_accept. */
kr_link *kr_link_from_fd(const kr_link_env *env, int fd, const char *id, int proto,
                         double now);
void kr_link_free(kr_link *l);

bool kr_link_pollfd(const kr_link *l, struct pollfd *p);
/* After the poll (revents of that pollfd, or 0) and on timeouts. Returns
 * KR_LINK_EV_* bits; *why says why it closed. */
int kr_link_pump(kr_link *l, short revents, double now, const char **why);
/* When the handshake gives up (0 once ready or closed). */
double kr_link_deadline(const kr_link *l);

bool kr_link_ready(const kr_link *l);
/* Did we open the TCP connection? */
bool kr_link_outgoing(const kr_link *l);
/* Did it close before TCP connected (so the peer may not reach us)? */
bool kr_link_connect_failed(const kr_link *l);

/* Known after the plain-text identity (outgoing: from the start). */
const char *kr_link_device_id(const kr_link *l);
int kr_link_protocol(const kr_link *l);
const struct sockaddr_in *kr_link_peer(const kr_link *l);
/* Once ready: the peer's full identity and certificate (borrowed). */
const kr_packet *kr_link_identity(const kr_link *l);
X509 *kr_link_cert(const kr_link *l);

/* The next packet received, or NULL; the caller frees it. Unparsable
 * lines are skipped. */
kr_packet *kr_link_next(kr_link *l);
/* Queues a packet; false if the link is not ready or broken. */
bool kr_link_send(kr_link *l, const kr_packet *p);

/* Our connection identity for target (shared with tests). */
kr_packet *kr_link_connection_identity(const kr_packet *identity, const char *target_id,
                                       int target_proto);

#endif
