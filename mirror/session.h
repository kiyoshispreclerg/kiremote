/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_SESSION_H
#define KR_SESSION_H

/* One kiremote-mirror per phone, whatever it does with it (the screen in a
 * window, or the camera): the process that runs listens on a Unix socket named
 * after the phone, $XDG_RUNTIME_DIR/kiremote-mirror/<key>.sock. Another one
 * for the same phone finds the socket and, instead of starting, brings the
 * window forward or says what the phone is busy with; the interfaces ask the
 * same before they start one. A socket left by a process that died is told
 * from a live one by connecting to it.
 *
 * The key is the KDE Connect device id when the interface knows it (--id),
 * else the adb serial. Protocol, one line each way: "status" answers the mode
 * ("mirror" or "camera"), "raise" and "quit" answer "ok" and call the owner's
 * callbacks. No libav, no X: the loop of the owner polls it. */

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    void (*on_raise)(void *ud);
    void (*on_quit)(void *ud);
} kr_inst_cbs;

typedef struct kr_inst kr_inst;

/* Takes the phone for this process. NULL if a live process has it: its mode
 * is then in other. NULL with other empty on any other trouble (no
 * directory, no memory): the caller goes on without being findable. */
kr_inst *kr_inst_claim(const char *key, const char *mode, const kr_inst_cbs *cbs, void *ud, char *other,
                       size_t other_size);
/* Closes and removes the socket. */
void kr_inst_free(kr_inst *i);

/* For the owner's poll(): the descriptors, then the answer to what they say. */
int kr_inst_pollfds(const kr_inst *i, struct pollfd *fds, int max);
void kr_inst_dispatch(kr_inst *i, const struct pollfd *fds, int n);

/* The other side. Is there a live process for key? Its mode, if so. Waits at
 * most a moment for the answer (the caller may be an interface). */
bool kr_inst_query(const char *key, char *mode, size_t size);
/* "raise" or "quit". True if the process answered ok. */
bool kr_inst_send(const char *key, const char *cmd);

#endif
