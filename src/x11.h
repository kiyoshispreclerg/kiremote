/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_X11_H
#define KR_X11_H

/* The X connection the daemon shares between plugins (clipboard, remote
 * input, the presenter's pointer): one Display, one hidden window to own
 * selections with, and the events fed to whoever registered for them. The
 * connection's descriptor goes into the daemon's poll() as a source (see
 * kr_source in daemon.h). Xlib only, no toolkit.
 *
 * Without $DISPLAY (or if the server refuses) there is no connection and
 * the plugins that need one stay quiet. Xlib's own error handlers would
 * kill the process on a protocol error; here errors are logged and
 * ignored, and the loss of the connection (the session ended) ends the
 * daemon, since Xlib offers no way to go on after that. */

#include <poll.h>
#include <stdbool.h>

#include <X11/Xlib.h>

typedef struct kr_x11 kr_x11;

/* Return true if the event was taken, so later handlers don't see it. */
typedef bool (*kr_x11_handler)(void *ud, XEvent *ev);

/* NULL (and a debug line) if there is no X server to talk to. */
kr_x11 *kr_x11_open(void);
void kr_x11_free(kr_x11 *x);

Display *kr_x11_display(const kr_x11 *x);
/* A 1x1 InputOnly window nobody sees, to receive selection replies on. */
Window kr_x11_window(const kr_x11 *x);

void kr_x11_add_handler(kr_x11 *x, kr_x11_handler fn, void *ud);
void kr_x11_remove_handler(kr_x11 *x, void *ud);

int kr_x11_pollfds(kr_x11 *x, struct pollfd *fds, int max);
/* Reads and hands out every pending event. */
void kr_x11_dispatch(kr_x11 *x, const struct pollfd *fds, int n);

#endif
