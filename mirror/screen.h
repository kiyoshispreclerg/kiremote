/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_SCREEN_H
#define KR_SCREEN_H

/* The mirror's window: plain Xlib. Each decoded frame is scaled to the
 * window by libswscale (keeping the aspect, black bars around) and put on
 * it through MIT-SHM, or XPutImage on a display without it. Redraws only
 * when a frame arrives or X asks (Expose), from the last frame kept. */

#include <stdbool.h>
#include <stddef.h>

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

/* What the user does in the window. Positions are already in the video's
 * pixels (clamped to it); state is X's modifier and button mask. */
typedef struct {
    /* X button 1 (left) to 9; down or up. */
    void (*on_button)(void *ud, int button, bool down, int x, int y, unsigned state);
    void (*on_motion)(void *ud, int x, int y, unsigned state);
    /* keysym as X looked it up; text: what the key typed (UTF-8, through
     * the input method, so dead keys and compose work), "" on release or
     * for keys that type nothing. repeat: the key is held (auto-repeat). */
    void (*on_key)(void *ud, unsigned long keysym, bool down, bool repeat, unsigned state, const char *text);
    /* The window lost the keyboard: whatever is held is let go. */
    void (*on_focus_out)(void *ud);
} kr_screen_input;

void kr_screen_set_input(kr_screen *s, const kr_screen_input *cbs, void *ud);

/* Brings the window forward (the window manager's say; it may only blink). */
void kr_screen_raise(kr_screen *s);

/* Fullscreen on and off (the window manager's, _NET_WM_STATE). */
void kr_screen_toggle_fullscreen(kr_screen *s);

/* ---- the computer's clipboard (the CLIPBOARD selection, text only) ---------- */

/* The most text taken from X; the phone takes less. */
#define KR_CLIP_MAX (256 * 1024)

/* on_changed: somebody else now holds the clipboard (they copied). It does
 * not say what the text is: kr_screen_clipboard_get does. Only a copy sends
 * anything: what was in the clipboard before the window opened, or when it
 * gets the keyboard, stays where it is (it may be a password). */
void kr_screen_clipboard_watch(kr_screen *s, void (*on_changed)(void *ud), void *ud);

/* Asks the holder for the text, UTF-8: cb comes later, once, with text NULL
 * if there is none (or it is a kind we don't take, like an INCR transfer of
 * something huge). A newer request replaces an older one. */
typedef void (*kr_clip_cb)(void *ud, const char *text, size_t len);
void kr_screen_clipboard_get(kr_screen *s, kr_clip_cb cb, void *ud);

/* Makes the window the holder of text (copied). It answers the others
 * until someone else copies. Text bigger than X takes in one property is
 * not offered. */
void kr_screen_clipboard_set(kr_screen *s, const char *text, size_t len);
/* The window holds the clipboard now. */
bool kr_screen_clipboard_mine(const kr_screen *s);

#endif
