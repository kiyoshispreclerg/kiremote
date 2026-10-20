/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CONTROL_H
#define KR_CONTROL_H

/* The control socket, $XDG_RUNTIME_DIR/kiremote.sock: a unix stream socket
 * where kiremote-cli (and later the UI) talk to the daemon, one JSON object
 * per line, each answered by one line:
 *
 *   {"type":"devices"}  → {"ok":true,"devices":[{"id","name","type",
 *                          "reachable","paired","state","key","address", and
 *                          while it is up: "plugins" (the ones that do something
 *                          with it), "battery":{"charge","charging"},
 *                          "signal":{"type","strength"}, "locked"...}]}
 *   {"type":"pair","device":"<id or name>"}     ask, or accept their request
 *   {"type":"unpair","device":...}              unpair, or refuse a request
 *   {"type":"ping","device":...,"message":"opt"}
 *   {"type":"share","device":...,"paths":["/abs/file",...]}
 *   {"type":"browse-files","device":...}        mount the phone's files and open them
 *   {"type":"unmount-files","device":...}
 *   {"type":"refresh"}                          announce ourselves again
 *   {"type":"version"}                          → {"ok":true,"version":"..."}
 *   {"type":"status"}                           → "name","type","id","version","pid" and
 *                                                 "devices" as above. The daemon also keeps
 *                                                 this object in kiremote-status.json next to
 *                                                 the socket (rewritten when it changes, removed
 *                                                 on exit) for programs that can't hold a socket
 *   {"type":"subscribe"}                        → {"ok":true}, and from then on
 *                                                 event lines (below); "unsubscribe" stops
 *   {"type":"config-schema"}                    → every setting: type, default, texts
 *   {"type":"config-get"}                       → {"values":{...},"set":[...]}
 *   {"type":"config-set","key":"a.b","value":"x"}   value null or "" = back to the default
 *
 * A bare word ("refresh") means {"type":"<word>"}. Replies are
 * {"ok":true,...} or {"ok":false,"error":"..."} (translated). The socket is
 * created with mode 0600: it can pair devices.
 *
 * Event lines have no "ok": {"event":"device","device":{...}} when a device
 * appears or anything about it changes, {"event":"removed","id":"..."},
 * {"event":"pair-request","device":{...}} and {"event":"config","key":"a.b",
 * "value":"x"}. A client that does not read them is dropped.
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

/* Sends line (no newline) as an event to every client that subscribed. */
void kr_control_broadcast(kr_control *c, const char *line);
bool kr_control_has_subscribers(const kr_control *c);

/* Marks reply as failed with error. */
void kr_control_fail(cJSON *reply, const char *error);

/* $XDG_RUNTIME_DIR/kiremote.sock, else /tmp/kiremote.sock-<uid>. */
bool kr_control_default_path(char *out, size_t size);

/* Client side, blocking (kiremote-cli): sends line, waits up to
 * timeout_ms for the reply line. The reply is malloc'd (caller frees);
 * NULL (errno set) if nobody listens or no reply came. */
char *kr_control_request(const char *path, const char *line, int timeout_ms);

/* Client side: a blocking connection for a program that stays (the CLI's
 * --watch, a UI with its own loop); -1 with errno if nobody listens. */
int kr_control_connect(const char *path);

#endif
