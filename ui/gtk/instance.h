/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_INSTANCE_H
#define KR_INSTANCE_H

/* One kiremote window per session. The first instance listens on a unix
 * socket; a later one hands its request over (show the window, open the
 * settings, ask a pairing question) and exits. The daemon does not start the
 * interface: it has to be open (or in the tray) to hear a pairing request. */

#include <glib.h>

/* The words a launch sends: "show", "settings", "pair=ID". */
typedef void (*kr_instance_fn)(void *ud, char **words);

/* Becomes the instance listening at path, or hands words (NULL-terminated)
 * to the one that is. TRUE: we are the instance (on_request gets what later
 * launches send). FALSE: another one has it and was told; exit. A path that
 * nobody listens on (left by a crash) is replaced. Without a way to listen
 * at all, it still returns TRUE (better two windows than none). */
gboolean kr_instance_acquire(const char *path, char **words, kr_instance_fn on_request, void *ud);
/* Stops listening and removes the socket. */
void kr_instance_release(void);

/* The path for a given control socket (NULL: the default): beside it,
 * "kiremote.sock" → "kiremote-ui.sock". g_free. */
char *kr_instance_path(const char *control_socket);

#endif
