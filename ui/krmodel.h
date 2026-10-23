/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_MODEL_H
#define KR_MODEL_H

/* What an interface knows about the daemon and the devices, and everything
 * it decides from that: the order of the list, the texts ("80% (charging)",
 * "LTE 3/4", "Connected"), which actions a device offers and the requests
 * they send. No GTK, no GLib, no I/O: the GTK window, the KiDesktop page
 * and the panel widget (which can't link GTK) all use this, so they cannot
 * disagree about what is shown.
 *
 * The model is fed by what the daemon says: a "status" reply (or the
 * status file), then the event lines (control.h). The kr_dev pointers it
 * hands out are valid until the next apply/load call. */

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#define KR_MODEL_PLUGINS 32

typedef struct {
    char id[64];
    char name[128];
    char type[16];     /* desktop, laptop, phone, tablet, tv */
    char state[24];    /* none, requested, requested_by_peer, paired */
    char key[12];      /* verification key while pairing, else "" */
    char address[48];
    bool reachable, paired;
    int battery;       /* 0..100, -1 unknown */
    bool charging;
    bool has_signal;
    char net_type[24]; /* "LTE", "5G"... */
    int signal;        /* 0..4, -1 no signal (valid if has_signal) */
    bool locked_known, locked;
    int n_plugins;     /* the plugins that do something with this device */
    char plugins[KR_MODEL_PLUGINS][32];
} kr_dev;

typedef struct kr_model kr_model;

kr_model *kr_model_new(void);
void kr_model_free(kr_model *m);

/* ---- the daemon ---- */

/* False while the daemon isn't there; then there are no devices. */
bool kr_model_daemon_up(const kr_model *m);
const char *kr_model_name(const kr_model *m); /* this computer's name; "" if unknown */
const char *kr_model_version(const kr_model *m);
/* The daemon went away (or never was): forget the devices. True if that
 * changed anything. */
bool kr_model_reset(kr_model *m);

/* ---- feeding it ---- */

/* A "status" reply or the status file's object. Replaces everything. False
 * if it isn't one. */
bool kr_model_apply_status(kr_model *m, const cJSON *status);

typedef enum {
    KR_EV_NONE,         /* not an event, or nothing to do */
    KR_EV_DEVICES,      /* a device appeared, changed or went away */
    KR_EV_PAIR_REQUEST, /* a device asks to pair: show the key (*id is its id) */
    KR_EV_CONFIG        /* a setting changed (*id is "section.key") */
} kr_event_kind;

/* One event line. id (may be NULL) gets the device or setting it is about,
 * valid until the next call. */
kr_event_kind kr_model_apply_event(kr_model *m, const cJSON *event, const char **id);

/* The status file written by the daemon (control.h), for programs that
 * can't hold a socket. Counts as "daemon down" when the file is missing or
 * its pid is gone. Returns true if the model changed. */
bool kr_model_load_file(kr_model *m, const char *path);
/* $XDG_RUNTIME_DIR/kiremote-status.json. */
bool kr_model_status_path(char *out, size_t size);

/* ---- the list ---- */

/* In the order to show: pairing requests, then connected, then paired but
 * away, then the others around; by name inside each. */
int kr_model_count(const kr_model *m);
const kr_dev *kr_model_at(const kr_model *m, int i);
const kr_dev *kr_model_find(const kr_model *m, const char *id);
/* Paired and reachable. */
int kr_model_connected(const kr_model *m);
/* Is someone asking to pair? */
bool kr_model_pair_pending(const kr_model *m);

/* ---- texts (translated) ---- */

/* "Connected", "Not reachable", "Wants to pair", "Available"... */
const char *kr_dev_state_text(const kr_dev *d);
/* "80%" or "80% (charging)"; "" if unknown. */
const char *kr_dev_battery_text(const kr_dev *d, char *buf, size_t size);
/* "LTE 3/4", "No signal"; "" if unknown. */
const char *kr_dev_signal_text(const kr_dev *d, char *buf, size_t size);
/* A themed icon name for the device's type. */
const char *kr_dev_icon(const kr_dev *d);
/* The panel tooltip: one line per connected device with its battery, or
 * why there is none. Lines are separated by '\n'. */
void kr_model_tooltip(const kr_model *m, char *buf, size_t size);

/* ---- actions ---- */

typedef enum {
    KR_ACT_PING,
    KR_ACT_FIND,       /* make the phone ring */
    KR_ACT_SEND_FILES, /* the host asks for the files, then kr_action_files */
    KR_ACT_SEND_TEXT,  /* the host asks for the text, then arg */
    KR_ACT_BROWSE,     /* its files in the file manager */
    KR_ACT_MIRROR,     /* its screen in kiremote-mirror (the host runs it; no request) */
    KR_ACT_PAIR,
    KR_ACT_ACCEPT,     /* their pairing request */
    KR_ACT_REFUSE,
    KR_ACT_CANCEL,     /* our pairing request */
    KR_ACT_UNPAIR,
    KR_ACT_REFRESH,    /* look for devices again (not about one device) */
    KR_ACT_COUNT
} kr_action;

/* Bitmask of (1u << kr_action): what this device offers right now. The
 * same in every interface. Never includes KR_ACT_REFRESH. */
unsigned kr_dev_actions(const kr_dev *d);
/* Is kiremote-mirror there to run? The host looks for the program and says
 * so; until then KR_ACT_MIRROR is never offered. */
void kr_actions_set_mirror(bool available);
/* "Send a ping", "Send files..."; ellipsis where the host must ask more. */
const char *kr_action_label(kr_action a);
/* Does the action need more from the user (files, text)? */
bool kr_action_needs_input(kr_action a);
/* Does it undo something, so that it deserves a confirmation? */
bool kr_action_destructive(kr_action a);
/* The request for the control socket; arg is the text for KR_ACT_SEND_TEXT.
 * The caller owns it (pass it to kr_client_send). NULL for actions that
 * need other input (files) or don't exist. */
cJSON *kr_action_request(kr_action a, const char *device_id, const char *arg);
cJSON *kr_action_files(const char *device_id, const char *const *paths, int n);

#endif
