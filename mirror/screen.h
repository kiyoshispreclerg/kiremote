/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_SCREEN_H
#define KR_SCREEN_H

/* The mirror's window: plain Xlib. Each decoded frame is scaled to the
 * window by libswscale (keeping the aspect, black bars around) and put on
 * it through MIT-SHM, or XPutImage on a display without it. Redraws only
 * when a frame arrives or X asks (Expose), from the last frame kept. */

#include <stdbool.h>

struct AVFrame;

typedef struct kr_screen kr_screen;

/* A window sized for a width x height video (shrunk to fit the screen).
 * title: UTF-8. NULL with *why (translated) if X cannot be used. */
kr_screen *kr_screen_new(const char *title, unsigned width, unsigned height, const char **why);
void kr_screen_free(kr_screen *s);

/* The X connection, for poll(). */
int kr_screen_fd(const kr_screen *s);
/* Handles what X sent. false once the user closed the window. */
bool kr_screen_dispatch(kr_screen *s);

/* The video changed size (rotation): the window takes the new shape. */
void kr_screen_video_size(kr_screen *s, unsigned width, unsigned height);
/* Draws a frame (kept for redrawing). */
void kr_screen_frame(kr_screen *s, const struct AVFrame *frame);

#endif
