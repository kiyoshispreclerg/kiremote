/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_BUS_H
#define KR_BUS_H

/* The session bus, for notifications and MPRIS. libdbus' watches and
 * timeouts are mapped onto the daemon's poll(), method calls are
 * asynchronous (a reply callback) and signals go to listeners, so nothing
 * here blocks after the connection is made (that handshake with the local
 * bus daemon is the one blocking step, at start-up).
 *
 * The address is $DBUS_SESSION_BUS_ADDRESS, else $XDG_RUNTIME_DIR/bus if
 * that socket exists. There is no autolaunch and no reconnection: without
 * a session bus (or after it goes away) the features that need it are
 * simply off.
 *
 * A kr_bus is a source of the daemon's loop (see kr_source in daemon.h). */

#include <poll.h>
#include <stdbool.h>

#include <dbus/dbus.h>

typedef struct kr_bus kr_bus;

/* reply is NULL when the call failed (error reply, no such service,
 * timeout). It is only valid during the callback. */
typedef void (*kr_bus_reply_fn)(void *ud, DBusMessage *reply);
typedef void (*kr_bus_signal_fn)(void *ud, DBusMessage *signal);

/* NULL (and a debug line) if there is no session bus. */
kr_bus *kr_bus_open(void);
void kr_bus_free(kr_bus *b);
/* False after the bus went away. */
bool kr_bus_alive(const kr_bus *b);

/* A method call to build arguments on with the dbus_message_* API. */
DBusMessage *kr_bus_new_call(const char *dest, const char *path, const char *iface,
                             const char *member);
/* Sends msg (consumed) and calls fn when the reply (or the failure)
 * comes. fn may be NULL when no reply is wanted. */
bool kr_bus_call(kr_bus *b, DBusMessage *msg, kr_bus_reply_fn fn, void *ud, int timeout_ms);

/* Signals matching rule (a D-Bus match rule string) go to fn; fn sees every
 * signal any listener asked for, so it checks the interface and member. */
bool kr_bus_listen(kr_bus *b, const char *rule, kr_bus_signal_fn fn, void *ud);
/* Stops delivering to this (fn, ud); the match rules stay. */
void kr_bus_unlisten(kr_bus *b, kr_bus_signal_fn fn, void *ud);

/* The loop's side. */
int kr_bus_pollfds(kr_bus *b, struct pollfd *fds, int max);
void kr_bus_dispatch(kr_bus *b, const struct pollfd *fds, int n);
/* Monotonic time of the next libdbus timeout, or now if data waits to be
 * dispatched; 0 for none. */
double kr_bus_deadline(const kr_bus *b);

#endif
