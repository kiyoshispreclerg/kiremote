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
#include <stddef.h>

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

/* Why a notification was closed (the Notifications spec's reasons). */
enum {
    KR_NOTE_EXPIRED = 1,
    KR_NOTE_DISMISSED = 2, /* the user closed it */
    KR_NOTE_CLOSED = 3,    /* by CloseNotification */
    KR_NOTE_UNKNOWN = 4,   /* also: no server */
};

typedef struct {
    void (*on_action)(void *ud, const char *key); /* optional */
    /* optional; the kr_note is gone after */
    void (*on_closed)(void *ud, int reason);
} kr_note_cb;

/* bus may be NULL (everything then goes to the log). */
kr_notify *kr_notify_new(kr_bus *bus);
void kr_notify_free(kr_notify *n);

bool kr_notify_has_actions(const kr_notify *n);
/* Does the server read markup in the body? Then &, < and > in text that is
 * not ours must be escaped (kr_notify_escape). */
bool kr_notify_has_markup(const kr_notify *n);
/* Copies in to out, escaping for markup when the server has it. */
void kr_notify_escape(const kr_notify *n, const char *in, char *out, size_t size);

/* Shows a notification. The returned handle is valid until on_closed, or
 * kr_note_detach; NULL when it only went to the log. cb may be NULL. */
kr_note *kr_notify_show(kr_notify *n, const kr_note_spec *spec, const kr_note_cb *cb, void *ud);
/* Replaces the text of a notification still shown (same slot, no new
 * popup). False if the server has not given it an id yet or it is gone;
 * then close it and show another. */
bool kr_note_update(kr_note *note, const kr_note_spec *spec);
/* Closes it (on_closed still follows if the server confirms). */
void kr_note_close(kr_note *note);
/* The owner is going away: no more callbacks; the notification stays. */
void kr_note_detach(kr_note *note);

#endif
