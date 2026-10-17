/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The phone as a laser pointer for presentations (plugins/presenter
 * upstream; and/plugins/presenter for the sender, which also sends the
 * slide keys as kdeconnect.mousepad.request, handled by the mousepad
 * plugin).
 *
 *   in  kdeconnect.presenter   {"dx", "dy"}   move the dot by that fraction
 *                                             of the screen (dy is scaled by
 *                                             the aspect ratio)
 *                              {"stop": true} hide it
 *
 * The dot is a small override-redirect window shaped to a circle with the
 * XShape extension, so it needs no compositor and no ARGB visual, and its
 * input region is empty: clicks go through to what is under it. Like the
 * upstream it starts in the middle of the screen, and disappears 0.5 s
 * after the last movement. The screen is the whole X screen (the upstream
 * uses the window's own screen under Qt; with several monitors the dot
 * ranges over all of them). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/extensions/shape.h>

#include "log.h"
#include "plugin.h"
#include "util.h"
#include "x11.h"

#define PRESENTER "kdeconnect.presenter"
#define HIDE_AFTER_S 0.5

static const char *const incoming[] = {PRESENTER, NULL};

static struct {
    kr_x11 *x;
    Display *dpy;
    Window win;
    int size;
    double xpos, ypos; /* 0..1 */
    double hide_at;
    bool source;
} pr;

static double num(const kr_packet *p, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p->body, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : 0;
}

static void hide(void)
{
    if (pr.win) {
        XDestroyWindow(pr.dpy, pr.win);
        XFlush(pr.dpy);
        pr.win = 0;
    }
    pr.hide_at = 0;
}

static unsigned long rgb(unsigned short r, unsigned short g, unsigned short b)
{
    XColor c = {.red = r, .green = g, .blue = b, .flags = DoRed | DoGreen | DoBlue};
    if (!XAllocColor(pr.dpy, DefaultColormap(pr.dpy, DefaultScreen(pr.dpy)), &c))
        return BlackPixel(pr.dpy, DefaultScreen(pr.dpy));
    return c.pixel;
}

static void show(void)
{
    int scr = DefaultScreen(pr.dpy);
    int w = DisplayWidth(pr.dpy, scr);
    pr.size = w / 150 < 12 ? 12 : w / 150;
    pr.xpos = pr.ypos = 0.5;

    XSetWindowAttributes attrs = {.override_redirect = True};
    pr.win = XCreateWindow(pr.dpy, DefaultRootWindow(pr.dpy), 0, 0, (unsigned)pr.size, (unsigned)pr.size, 0,
                           CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect, &attrs);

    /* The look: a red disc with a white rim, drawn once as the background. */
    Pixmap look = XCreatePixmap(pr.dpy, pr.win, (unsigned)pr.size, (unsigned)pr.size, (unsigned)DefaultDepth(pr.dpy, scr));
    GC gc = XCreateGC(pr.dpy, look, 0, NULL);
    XSetForeground(pr.dpy, gc, WhitePixel(pr.dpy, scr));
    XFillArc(pr.dpy, look, gc, 0, 0, (unsigned)pr.size, (unsigned)pr.size, 0, 360 * 64);
    XSetForeground(pr.dpy, gc, rgb(0xffff, 0, 0));
    XFillArc(pr.dpy, look, gc, 1, 1, (unsigned)pr.size - 2, (unsigned)pr.size - 2, 0, 360 * 64);
    XSetWindowBackgroundPixmap(pr.dpy, pr.win, look);
    XFreeGC(pr.dpy, gc);
    XFreePixmap(pr.dpy, look);

    /* The shape: a circle. */
    Pixmap mask = XCreatePixmap(pr.dpy, pr.win, (unsigned)pr.size, (unsigned)pr.size, 1);
    GC mgc = XCreateGC(pr.dpy, mask, 0, NULL);
    XSetForeground(pr.dpy, mgc, 0);
    XFillRectangle(pr.dpy, mask, mgc, 0, 0, (unsigned)pr.size, (unsigned)pr.size);
    XSetForeground(pr.dpy, mgc, 1);
    XFillArc(pr.dpy, mask, mgc, 0, 0, (unsigned)pr.size, (unsigned)pr.size, 0, 360 * 64);
    XShapeCombineMask(pr.dpy, pr.win, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(pr.dpy, mgc);
    XFreePixmap(pr.dpy, mask);
    /* No input region: clicks pass through. */
    XShapeCombineRectangles(pr.dpy, pr.win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);

    XMapWindow(pr.dpy, pr.win);
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)d;
    (void)dev;
    if (!pr.x)
        return;
    if (kr_packet_bool(p, "stop", false)) {
        hide();
        return;
    }
    if (!pr.win)
        show();
    int scr = DefaultScreen(pr.dpy);
    int w = DisplayWidth(pr.dpy, scr), h = DisplayHeight(pr.dpy, scr);
    double ratio = (double)w / h;
    pr.xpos += num(p, "dx");
    pr.ypos += num(p, "dy") * ratio;
    pr.xpos = pr.xpos < 0 ? 0 : pr.xpos > 1 ? 1 : pr.xpos;
    pr.ypos = pr.ypos < 0 ? 0 : pr.ypos > 1 ? 1 : pr.ypos;
    XMoveWindow(pr.dpy, pr.win, (int)(pr.xpos * w) - pr.size / 2, (int)(pr.ypos * h) - pr.size / 2);
    XRaiseWindow(pr.dpy, pr.win);
    XFlush(pr.dpy);
    pr.hide_at = kr_now() + HIDE_AFTER_S;
}

static double src_deadline(void *ud)
{
    (void)ud;
    return pr.hide_at;
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    (void)ud;
    (void)fds;
    (void)n;
    if (pr.hide_at && kr_now() >= pr.hide_at)
        hide();
}

static void init(kr_daemon *d)
{
    kr_x11 *x = kr_daemon_x11(d);
    if (!x)
        return;
    int ev, err;
    if (!XShapeQueryExtension(kr_x11_display(x), &ev, &err)) {
        kr_log_warn("the X server has no Shape extension: the presentation pointer is off");
        return;
    }
    pr.x = x;
    pr.dpy = kr_x11_display(x);
    kr_daemon_add_source(d, &(kr_source){NULL, src_dispatch, src_deadline}, &pr);
    pr.source = true;
}

static void on_disconnect(kr_daemon *d, kr_device *dev)
{
    (void)d;
    (void)dev;
    if (pr.x)
        hide(); /* a pointer left behind by a dead connection */
}

static void shutdown_plugin(kr_daemon *d)
{
    if (pr.x)
        hide();
    if (pr.source)
        kr_daemon_remove_source(d, &pr);
    memset(&pr, 0, sizeof pr);
}

const kr_plugin kr_plugin_presenter = {
    .name = "presenter",
    .incoming = incoming,
    .init = init,
    .shutdown = shutdown_plugin,
    .on_disconnect = on_disconnect,
    .on_packet = on_packet,
};
