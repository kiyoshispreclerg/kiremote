/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_MPRIS_H
#define KR_MPRIS_H

/* The MPRIS media players on the session bus (VLC, mpv, Spotify, browsers):
 * who is there, what they play, and a way to drive them. Used by the
 * mpriscontrol plugin (the phone controls the desktop's players) and by
 * pausemusic (pause while a call rings).
 *
 * Nothing blocks. ListNames and the per-player GetAll are asynchronous;
 * NameOwnerChanged, PropertiesChanged and Seeked keep the model current.
 * A player shows up (on_list) only once its properties have arrived.
 *
 * Follows kdeconnect-kde's plugins/mpriscontrol (which copied the
 * plasma-workspace mpris2 engine): players named "org.mpris.MediaPlayer2.*",
 * except "kdeconnect.*" (our own kind of bridge) and "playerctld" (a proxy
 * of the others). */

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

typedef struct kr_mpris kr_mpris;

typedef struct {
    char service[128]; /* org.mpris.MediaPlayer2.vlc */
    char owner[64];    /* unique bus name the signals come from */
    char name[160];    /* what we call it: the player's Identity, made unique */
    bool loaded;
    char status[16];   /* Playing, Paused or Stopped */
    char title[256], artist[256], album[256], art_url[512], url[512];
    int64_t length_ms; /* -1 if unknown */
    int64_t pos_ms;    /* last known; kr_mpris_get_position for a fresh one */
    double volume;     /* 0..1, -1 if the player has none */
    bool can_pause, can_play, can_next, can_prev, can_seek;
    bool has_loop, has_shuffle;
    char loop[16];     /* None, Track or Playlist */
    bool shuffle;
} kr_player;

/* What changed (bits for on_change). */
enum {
    KR_MP_STATUS = 1 << 0,
    KR_MP_META = 1 << 1,
    KR_MP_VOLUME = 1 << 2,
    KR_MP_LOOP = 1 << 3,
    KR_MP_SHUFFLE = 1 << 4,
    KR_MP_CAN = 1 << 5,
};

typedef struct {
    /* A player appeared, went away or was renamed. */
    void (*on_list)(void *ud);
    /* Properties changed (KR_MP_* bits). */
    void (*on_change)(void *ud, const kr_player *p, unsigned changed);
    /* The position jumped. */
    void (*on_seeked)(void *ud, const kr_player *p);
} kr_mpris_listener;

/* Starts tracking on bus (which may be NULL: then there are no players). */
kr_mpris *kr_mpris_new(kr_bus *bus);
void kr_mpris_free(kr_mpris *m);

void kr_mpris_add_listener(kr_mpris *m, const kr_mpris_listener *l, void *ud);
void kr_mpris_remove_listener(kr_mpris *m, void *ud);

int kr_mpris_count(const kr_mpris *m); /* loaded players */
const kr_player *kr_mpris_at(const kr_mpris *m, int i);
const kr_player *kr_mpris_find(const kr_mpris *m, const char *name);

/* ---- driving a player ---- */

/* Play, Pause, PlayPause, Stop, Next or Previous: anything else is refused,
 * unlike the upstream, which calls whatever the phone names. */
bool kr_mpris_action(kr_mpris *m, const kr_player *p, const char *action);
bool kr_mpris_set_volume(kr_mpris *m, const kr_player *p, double volume);
/* None, Track or Playlist. */
bool kr_mpris_set_loop(kr_mpris *m, const kr_player *p, const char *loop);
bool kr_mpris_set_shuffle(kr_mpris *m, const kr_player *p, bool on);
/* Relative seek, in microseconds, as MPRIS wants it. */
bool kr_mpris_seek(kr_mpris *m, const kr_player *p, int64_t offset_us);

/* Asks the player for its position; cb gets the player's name and the
 * position in ms (-1 on failure) when it answers. The player may be gone
 * by then: cb looks it up again. */
typedef void (*kr_mpris_pos_fn)(void *ud, const char *player, int64_t pos_ms);
bool kr_mpris_get_position(kr_mpris *m, const kr_player *p, kr_mpris_pos_fn cb, void *ud);

/* For tests: feed a message in as if the bus had delivered it. */
void kr_mpris_handle_signal(kr_mpris *m, DBusMessage *msg);

#endif
