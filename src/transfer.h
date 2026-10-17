/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_TRANSFER_H
#define KR_TRANSFER_H

/* Payloads (files) travel on their own TLS connections, beside the link
 * (docs/PROTOCOL.md, "Payloads"):
 *
 *   upload: we listen on a port from 1739-1764, announce it in the
 *     packet's payloadTransferInfo, and serve the bytes as TLS *server*
 *     to whoever connects with the paired device's certificate;
 *   download: we connect to the port the packet names, as TLS *client*,
 *     and read payloadSize bytes into a file.
 *
 * Both run in the daemon's poll() like links. A download writes to
 * "<name>.part" and only takes its final name, made unique (" (1)", ...),
 * when complete. A transfer owns its sockets and file descriptor. */

#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#include <openssl/ssl.h>

typedef struct kr_transfer kr_transfer;

/* Seconds a transfer may wait for its connection, or go without
 * progress, before it gives up (CompositeUploadJob waits 30 s). */
#define KR_TRANSFER_TIMEOUT_S 30.0

/* Listens on a free payload port (*port) to serve size bytes of fd (taken
 * over) to the holder of peer_cert. NULL (fd closed) if no port is free. */
kr_transfer *kr_transfer_upload(SSL_CTX *ctx, X509 *peer_cert, int fd, int64_t size,
                                const char *name, double now, int *port);

/* Connects to addr (the link's peer, with the packet's port) and saves
 * size bytes (-1: until the peer closes) as path, made unique. mtime_ms
 * (0: leave it) is set on the file at the end. */
kr_transfer *kr_transfer_download(SSL_CTX *ctx, X509 *peer_cert, const struct sockaddr_in *addr,
                                  int64_t size, const char *path, int64_t mtime_ms, double now);
void kr_transfer_free(kr_transfer *t);

bool kr_transfer_pollfd(const kr_transfer *t, struct pollfd *p);

enum {
    KR_TRANSFER_RUNNING,
    KR_TRANSFER_DONE,
    KR_TRANSFER_FAILED,
};
/* After the poll (revents of that pollfd, or 0) and on timeouts. *why
 * says why it failed. */
int kr_transfer_pump(kr_transfer *t, short revents, double now, const char **why);
double kr_transfer_deadline(const kr_transfer *t);

/* The file name (uploads) or final path (downloads, once done). */
const char *kr_transfer_name(const kr_transfer *t);
bool kr_transfer_is_upload(const kr_transfer *t);

/* path, or "path (1).ext", "path (2).ext"... whichever doesn't exist,
 * into out (for tests too). */
bool kr_unique_path(const char *path, int n, char *out, size_t size);

#endif
