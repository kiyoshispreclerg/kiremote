/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CLIENT_H
#define KR_CLIENT_H

/* An interface's connection to kiremoted (control.h): it subscribes to the
 * events, keeps a kr_model current, sends requests and calls back with the
 * replies, and reconnects by itself when the daemon goes away or comes
 * later.
 *
 * It never blocks and owns no loop: the host (GLib's main loop in the GTK
 * programs, poll() in the tests) watches kr_client_fd() for
 * kr_client_events(), calls kr_client_dispatch() when it is ready and
 * kr_client_tick() at kr_client_deadline(). No GLib here, so it is tested
 * without a display. Not thread safe; callbacks must not free the client. */

#include <stdbool.h>

#include "cJSON.h"
#include "krmodel.h"

typedef enum {
    KR_CH_DAEMON,       /* the daemon appeared or went away: kr_model_daemon_up() */
    KR_CH_DEVICES,      /* the list changed (id: the device, or NULL for all) */
    KR_CH_PAIR_REQUEST, /* a device asks to pair (id) */
    KR_CH_CONFIG        /* a setting changed (id: "section.key") */
} kr_change;

typedef struct {
    void (*on_change)(void *ud, kr_change what, const char *id);
} kr_client_ops;

typedef struct kr_client kr_client;

/* path NULL: the default socket. Tries to connect at once (and again
 * later, whatever happens). */
kr_client *kr_client_new(const char *path, const kr_client_ops *ops, void *ud);
void kr_client_free(kr_client *c);

/* Updated by the events; valid as long as the client. */
const kr_model *kr_client_model(const kr_client *c);

/* A reply, or NULL if the connection went away first (or never was). A
 * reply with "ok":false is still a reply: see kr_reply_error. */
typedef void (*kr_reply_fn)(void *ud, const cJSON *reply);

/* Sends req (taken over) and, if cb is not NULL, calls it with the reply.
 * False (and cb(ud, NULL) at once) when not connected. */
bool kr_client_send(kr_client *c, cJSON *req, kr_reply_fn cb, void *ud);

/* NULL if reply is ok, else the daemon's message (or a generic one for NULL). */
const char *kr_reply_error(const cJSON *reply);

/* ---- for the host's loop ---- */

/* The descriptor to watch, or -1 when not connected. */
int kr_client_fd(const kr_client *c);
/* poll() events to ask for: POLLIN, plus POLLOUT while there is output waiting. */
short kr_client_events(const kr_client *c);
void kr_client_dispatch(kr_client *c, short revents);
/* kr_now() time for the next reconnection attempt; 0 when connected. */
double kr_client_deadline(const kr_client *c);
void kr_client_tick(kr_client *c, double now);

#endif
