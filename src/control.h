/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CONTROL_H
#define KR_CONTROL_H

/* The control socket, $XDG_RUNTIME_DIR/kiremote.sock: a unix stream socket
 * where kiremote-cli (and later the UI) talk to the daemon, one JSON object
 * per line, each answered by one line:
 *
 *   {"type":"devices"}  → {"ok":true,"devices":[{"id","name","type",
 *                          "reachable","paired","state","key","address"}]}
 *   {"type":"pair","device":"<id or name>"}     ask, or accept their request
 *   {"type":"unpair","device":...}              unpair, or refuse a request
 *   {"type":"ping","device":...,"message":"opt"}
 *   {"type":"share","device":...,"paths":["/abs/file",...]}
 *   {"type":"refresh"}                          announce ourselves again
 *   {"type":"version"}                          → {"ok":true,"version":"..."}
 *
 * A bare word ("refresh") means {"type":"<word>"}. Replies are
 * {"ok":true,...} or {"ok":false,"error":"..."} (translated). The socket is
 * created with mode 0600: it can pair devices.
 *
 * Everything runs in the main loop: the socket and its clients are polled
 * with the rest, and nothing here blocks. Adapted from kikarinhas. */

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

typedef struct kr_control kr_control;

/* Handles every request. reply already holds "ok": true; call
 * kr_control_fail or add other fields. */
typedef void (*kr_control_fn)(void *ud, const char *type, const cJSON *req, cJSON *reply);

/* Creates the socket at path. If another daemon answers there, fails
 * (NULL) and leaves it alone; a stale socket file is replaced. With path
 * NULL there is no socket, only kr_control_handle_line (tests). */
kr_control *kr_control_open(const char *path, kr_control_fn on_request, void *ud);
/* Closes the clients and removes the socket file. */
void kr_control_close(kr_control *c);

/* Appends the descriptors to poll (listener and clients) to fds, at most
 * max; returns how many. */
int kr_control_pollfds(const kr_control *c, struct pollfd *fds, int max);
/* After the poll: accepts, reads and answers whatever is ready. */
void kr_control_dispatch(kr_control *c, const struct pollfd *fds, int n);

/* Handles one request line as if a client sent it; returns the reply line
 * (without "\n"), which the caller frees. */
char *kr_control_handle_line(kr_control *c, const char *line);

/* Marks reply as failed with error. */
void kr_control_fail(cJSON *reply, const char *error);

/* $XDG_RUNTIME_DIR/kiremote.sock, else /tmp/kiremote.sock-<uid>. */
bool kr_control_default_path(char *out, size_t size);

/* Client side, blocking (kiremote-cli): sends line, waits up to
 * timeout_ms for the reply line. The reply is malloc'd (caller frees);
 * NULL (errno set) if nobody listens or no reply came. */
char *kr_control_request(const char *path, const char *line, int timeout_ms);

#endif
