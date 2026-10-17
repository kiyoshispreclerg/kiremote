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

#include "cJSON.h"
#include "config.h"
#include "packet.h"

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

/* ---- for plugins ---- */

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
/* Sends p with fd (taken over, size bytes) as its payload. */
bool kr_daemon_upload(kr_daemon *d, kr_device *dev, kr_packet *p, int fd, int64_t size,
                      const char *name);

#endif
