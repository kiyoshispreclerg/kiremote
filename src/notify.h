/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_NOTIFY_H
#define KR_NOTIFY_H

/* Desktop notifications over org.freedesktop.Notifications. Without a bus
 * or a server the text goes to the log instead, so plugins always call this
 * and never care.
 *
 * Actions (buttons) are only offered when the server says it supports them
 * (GetCapabilities): the xispanel server does not, and then the same thing
 * has to be reachable some other way (the control socket, later the
 * interface). kr_notify_has_actions says which case this is. */

#include <stdbool.h>

#include "bus.h"

typedef struct kr_notify kr_notify;
typedef struct kr_note kr_note;

typedef struct {
    const char *summary;
    const char *body;       /* may be NULL */
    const char *icon;       /* icon name, may be NULL */
    const char *const *actions; /* {"key", "Label", ..., NULL}; may be NULL */
    int timeout_ms;         /* -1: server default, 0: stays until closed */
    bool urgent;            /* urgency "critical" */
} kr_note_spec;

typedef struct {
    void (*on_action)(void *ud, const char *key); /* optional */
    void (*on_closed)(void *ud);                  /* optional; the kr_note is gone after */
} kr_note_cb;

/* bus may be NULL (everything then goes to the log). */
kr_notify *kr_notify_new(kr_bus *bus);
void kr_notify_free(kr_notify *n);

bool kr_notify_has_actions(const kr_notify *n);

/* Shows a notification. The returned handle is valid until on_closed, or
 * kr_note_detach; NULL when it only went to the log. cb may be NULL. */
kr_note *kr_notify_show(kr_notify *n, const kr_note_spec *spec, const kr_note_cb *cb, void *ud);
/* Closes it (on_closed still follows if the server confirms). */
void kr_note_close(kr_note *note);
/* The owner is going away: no more callbacks; the notification stays. */
void kr_note_detach(kr_note *note);

#endif
