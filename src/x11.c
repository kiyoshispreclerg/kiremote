/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "x11.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define MAX_HANDLERS 8

typedef struct {
    kr_x11_handler fn;
    void *ud;
} handler;

struct kr_x11 {
    Display *dpy;
    Window win;
    handler handlers[MAX_HANDLERS];
    int n_handlers;
};

static int on_error(Display *dpy, XErrorEvent *e)
{
    char text[128];
    XGetErrorText(dpy, e->error_code, text, sizeof text);
    kr_log_debug("X error: %s (request %d, resource 0x%lx)", text, e->request_code, e->resourceid);
    return 0;
}

static int on_io_error(Display *dpy)
{
    (void)dpy;
    /* The server went away: Xlib cannot continue and this must not return. */
    kr_log_error("lost the connection to the X server");
    _exit(1);
}

kr_x11 *kr_x11_open(void)
{
    const char *name = getenv("DISPLAY");
    if (!name || !*name) {
        kr_log_debug("no $DISPLAY: X features are off");
        return NULL;
    }
    XSetErrorHandler(on_error);
    XSetIOErrorHandler(on_io_error);
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        kr_log_debug("cannot open the display %s", name);
        return NULL;
    }
    kr_x11 *x = calloc(1, sizeof *x);
    if (!x) {
        XCloseDisplay(dpy);
        return NULL;
    }
    x->dpy = dpy;
    XSetWindowAttributes attrs = {.override_redirect = True};
    x->win = XCreateWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, InputOnly, CopyFromParent,
                           CWOverrideRedirect, &attrs);
    XFlush(dpy);
    return x;
}

void kr_x11_free(kr_x11 *x)
{
    if (!x)
        return;
    XDestroyWindow(x->dpy, x->win);
    XCloseDisplay(x->dpy);
    free(x);
}

Display *kr_x11_display(const kr_x11 *x)
{
    return x->dpy;
}

Window kr_x11_window(const kr_x11 *x)
{
    return x->win;
}

void kr_x11_add_handler(kr_x11 *x, kr_x11_handler fn, void *ud)
{
    if (x->n_handlers < MAX_HANDLERS)
        x->handlers[x->n_handlers++] = (handler){fn, ud};
}

void kr_x11_remove_handler(kr_x11 *x, void *ud)
{
    for (int i = 0; i < x->n_handlers;) {
        if (x->handlers[i].ud == ud)
            x->handlers[i] = x->handlers[--x->n_handlers];
        else
            i++;
    }
}

int kr_x11_pollfds(kr_x11 *x, struct pollfd *fds, int max)
{
    if (max < 1)
        return 0;
    fds[0] = (struct pollfd){.fd = ConnectionNumber(x->dpy), .events = POLLIN};
    return 1;
}

void kr_x11_dispatch(kr_x11 *x, const struct pollfd *fds, int n)
{
    (void)fds;
    (void)n;
    /* Events can already be queued in Xlib while the descriptor is quiet
     * (a reply read together with a request), so always drain. */
    while (XPending(x->dpy) > 0) {
        XEvent ev;
        XNextEvent(x->dpy, &ev);
        for (int i = 0; i < x->n_handlers; i++)
            if (x->handlers[i].fn(x->handlers[i].ud, &ev))
                break;
    }
    XFlush(x->dpy);
}
