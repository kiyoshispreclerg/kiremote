/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_SERVER_H
#define KR_SERVER_H

/* Starts the scrcpy-server on the phone and hands over its video socket
 * (docs/PROTOCOL.md, "Espelhamento"): `adb devices -l` to pick the phone,
 * `adb push` of the jar (before that, optionally `adb connect` or the switch to TCP/IP), a local TCP port on 127.0.0.1 behind an `adb
 * reverse` tunnel, then `adb shell ... app_process` with the options, and
 * the server connects back. Every step is a child process of the caller's
 * loop (kr_procs) or the listening socket: nothing blocks.
 *
 * The kr_server owns the shell that runs the server: freeing it stops the
 * server (and removes the tunnel if it is still there). */

#include <poll.h>
#include <stdbool.h>

#include "proc.h"

typedef struct {
    const char *serial;   /* NULL: the only phone adb has */
    const char *jar;      /* the scrcpy-server to push */
    unsigned max_size;    /* longer side, 0 = the phone's */
    unsigned bit_rate;    /* 0 = the server's default */
    unsigned max_fps;     /* 0 = no limit */
    const char *codec;    /* "h264" (default), "h265", "av1" */
    double timeout;       /* seconds for the server to connect, 0 = 10 */
    bool view_only;       /* no control socket: the phone is only shown */
    bool audio;           /* raw PCM from the phone (Android 11 or newer) */
    bool stay_awake;      /* the phone does not sleep while it is charging */
    bool camera;          /* the phone's camera instead of its screen (Android 12 or newer) */
    const char *camera_facing; /* "back" (default), "front" or "external" */
    /* Over the network (no cable): */
    const char *connect;  /* HOST or HOST:PORT (port 5555 if none): `adb connect` it */
    bool connect_fallback; /* ...only when adb lists no phone (a cable wins) */
    bool tcpip;           /* the phone is on USB: switch it to TCP/IP and use that, so the cable can go */
} kr_server_opts;

typedef struct {
    /* The sockets are connected (blocking mode; the caller owns them):
     * video, audio (-1 without opts.audio) and control (-1 with view_only).
     * An old phone still has an audio socket; it says there is no audio in
     * its first bytes (kr_stream_disabled). serial and model: the phone adb
     * picked. */
    void (*on_ready)(void *ud, int video_fd, int audio_fd, int control_fd, const char *serial,
                     const char *model);
    /* It could not start, or the server ended (before or after on_ready):
     * a translated sentence. Comes once; nothing else follows. */
    void (*on_error)(void *ud, const char *message);
    /* A line the server printed (its log). Optional. */
    void (*on_log)(void *ud, const char *line);
} kr_server_cb;

typedef struct kr_server kr_server;

kr_server *kr_server_start(kr_procs *ps, const kr_server_opts *opts, const kr_server_cb *cb, void *ud);
void kr_server_free(kr_server *s);

/* The listening socket, while the server is expected. */
int kr_server_pollfds(kr_server *s, struct pollfd *fds, int max);
void kr_server_dispatch(kr_server *s, const struct pollfd *fds, int n);
/* When the wait for the server runs out (kr_now() time), 0 for none. */
double kr_server_deadline(const kr_server *s);
/* Call when the deadline passed. */
void kr_server_tick(kr_server *s);

/* Where the jar is: $KIREMOTE_SCRCPY_SERVER, next to the executable (a
 * build tree), else the installed one. false if none exists. */
bool kr_server_find_jar(char *out, size_t size);

#endif
