/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_DAEMON_H
#define KR_DAEMON_H

/* The daemon's core: the LAN backend (discovery on UDP 1716, links on TCP
 * 1716-1764), the devices (known by id, trusted or just visible), pairing,
 * payload transfers, plugins and the control socket. kiremoted.c owns the
 * poll() loop and calls in here; nothing blocks.
 *
 * A device lives while it is trusted, connected, or pairing; the daemon
 * owns it. Plugins get kr_device pointers that stay valid for the duration
 * of the call. */

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "cJSON.h"
#include "config.h"
#include "mpris.h"
#include "notify.h"
#include "packet.h"
#include "proc.h"
#include "x11.h"

typedef struct kr_daemon kr_daemon;
typedef struct kr_device kr_device;

/* Sets everything up and announces us. socket_path NULL: the default.
 * cfg must outlive the daemon. NULL on failure (logged). */
kr_daemon *kr_daemon_new(const kr_config *cfg, const char *socket_path);
void kr_daemon_free(kr_daemon *d);

/* Fills fds (at most max) with what to poll; returns how many. */
int kr_daemon_pollfds(kr_daemon *d, struct pollfd *fds, int max);
/* After the poll, with the fds kr_daemon_pollfds filled. */
void kr_daemon_dispatch(kr_daemon *d, const struct pollfd *fds, int n);
/* Milliseconds until the next timer, -1 for none. */
int kr_daemon_timeout(const kr_daemon *d);

/* Sends our discovery packet again (SIGHUP, "refresh", network change). */
void kr_daemon_announce(kr_daemon *d);

/* ---- sources: more things to poll ---- */

/* Anything with descriptors and timers that belongs in the loop (the bus,
 * the child processes, a plugin's watcher). */
typedef struct {
    /* Fills fds with at most max descriptors; returns how many. */
    int (*pollfds)(void *ud, struct pollfd *fds, int max);
    /* After every poll, with the descriptors it filled (n may be 0). */
    void (*dispatch)(void *ud, const struct pollfd *fds, int n);
    /* Monotonic time (kr_now) it wants to run at, 0 for none; optional. */
    double (*deadline)(void *ud);
} kr_source;

/* Copies s. False without memory. Sources are dispatched in the order they
 * were added. */
bool kr_daemon_add_source(kr_daemon *d, const kr_source *s, void *ud);
void kr_daemon_remove_source(kr_daemon *d, void *ud);

/* Shared services; the bus and the notifier work (as logging) even if
 * there is no session bus, but the bus itself is then NULL. */
kr_bus *kr_daemon_bus(kr_daemon *d);
kr_notify *kr_daemon_notify(kr_daemon *d);
kr_procs *kr_daemon_procs(kr_daemon *d);
/* The system bus (logind), opened on the first call; NULL if there is none. */
kr_bus *kr_daemon_system_bus(kr_daemon *d);
/* The shared X connection, opened on the first call; NULL without $DISPLAY. */
kr_x11 *kr_daemon_x11(kr_daemon *d);
/* The MPRIS players, tracked from the first call on (NULL without a bus). */
kr_mpris *kr_daemon_mpris(kr_daemon *d);

/* ---- for plugins ---- */

/* The paired device with that id, or NULL. */
kr_device *kr_daemon_find_device(kr_daemon *d, const char *id);
/* The paired, reachable devices: how many, and the i-th. */
int kr_daemon_device_count(const kr_daemon *d);
kr_device *kr_daemon_device_at(const kr_daemon *d, int i);

/* The device named by req's "device" field (an id, or a unique name); on
 * failure replies with an error and returns NULL. For plugins' requests. */
kr_device *kr_daemon_request_device(kr_daemon *d, const cJSON *req, cJSON *reply);

const char *kr_device_id(const kr_device *dev);
const char *kr_device_name(const kr_device *dev);
bool kr_device_paired(const kr_device *dev);
bool kr_device_reachable(const kr_device *dev);

const kr_config *kr_daemon_config(const kr_daemon *d);

/* Sends a packet to a paired, reachable device, if it takes that type
 * (its incomingCapabilities). */
bool kr_daemon_send(kr_daemon *d, kr_device *dev, const kr_packet *p);

/* Fetches the payload of p (received from dev) into path. */
bool kr_daemon_download(kr_daemon *d, kr_device *dev, const kr_packet *p, const char *path,
                        int64_t mtime_ms);
/* The same, and done(ud, ok, final_path) when it ends; final_path (valid
 * during the call) may differ from path if that already existed. done is
 * also called when the transfer fails, but not if the daemon is going away. */
typedef void (*kr_download_done_fn)(void *ud, bool ok, const char *final_path);
bool kr_daemon_download_cb(kr_daemon *d, kr_device *dev, const kr_packet *p, const char *path,
                           int64_t mtime_ms, kr_download_done_fn done, void *ud);
/* Sends p with fd (taken over, size bytes) as its payload. */
bool kr_daemon_upload(kr_daemon *d, kr_device *dev, kr_packet *p, int fd, int64_t size,
                      const char *name);

#endif
