/* SPDX-License-Identifier: GPL-3.0-or-later */
/* X helper for tests/net/desktop.sh; always run against the test's own
 * Xvfb.
 *
 *   xtool events                prints every key, button and motion event the
 *                               root window gets, one line each, until killed
 *   xtool own TEXT [--secret]   owns the CLIPBOARD and serves TEXT as
 *                               UTF8_STRING (--secret also offers the
 *                               password manager hint target); until killed
 *   xtool windows               one line per mapped top-level window:
 *                               "win WxH+X+Y inputrects=N"
 *   xtool pointer               "pointer X Y" */
#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

static void events(Display *dpy)
{
    Window root = DefaultRootWindow(dpy);
    XSelectInput(dpy, root, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XFlush(dpy);
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        char buf[16];
        KeySym ks = 0;
        switch (ev.type) {
        case KeyPress:
        case KeyRelease:
            XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
            printf("%s sym=0x%lx\n", ev.type == KeyPress ? "KeyPress" : "KeyRelease", (unsigned long)ks);
            break;
        case ButtonPress:
        case ButtonRelease:
            printf("%s %u\n", ev.type == ButtonPress ? "ButtonPress" : "ButtonRelease", ev.xbutton.button);
            break;
        case MotionNotify:
            printf("Motion %d %d\n", ev.xmotion.x_root, ev.xmotion.y_root);
            break;
        default:
            continue;
        }
        fflush(stdout);
    }
}

static void own(Display *dpy, const char *text, bool secret)
{
    Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False), utf8 = XInternAtom(dpy, "UTF8_STRING", False),
         targets = XInternAtom(dpy, "TARGETS", False), hint = XInternAtom(dpy, "x-kde-passwordManagerHint", False);
    XSetSelectionOwner(dpy, clipboard, w, CurrentTime);
    XFlush(dpy);
    printf("owning\n");
    fflush(stdout);
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type != SelectionRequest)
            continue;
        XSelectionRequestEvent *rq = &ev.xselectionrequest;
        XSelectionEvent n = {.type = SelectionNotify, .display = dpy, .requestor = rq->requestor,
                             .selection = rq->selection, .target = rq->target, .property = None, .time = rq->time};
        Atom prop = rq->property ? rq->property : rq->target;
        if (rq->target == targets) {
            Atom list[] = {targets, utf8, hint};
            XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)list,
                            secret ? 3 : 2);
            n.property = prop;
        } else if (rq->target == utf8) {
            XChangeProperty(dpy, rq->requestor, prop, utf8, 8, PropModeReplace, (const unsigned char *)text,
                            (int)strlen(text));
            n.property = prop;
        }
        XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&n);
        XFlush(dpy);
    }
}

static int windows(Display *dpy)
{
    Window root, parent, *kids;
    unsigned n;
    XQueryTree(dpy, DefaultRootWindow(dpy), &root, &parent, &kids, &n);
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy, kids[i], &a) || a.map_state != IsViewable || a.class == InputOnly)
            continue;
        int count = 0, ordering;
        XRectangle *r = XShapeGetRectangles(dpy, kids[i], ShapeInput, &count, &ordering);
        if (r)
            XFree(r);
        printf("win %dx%d+%d+%d inputrects=%d\n", a.width, a.height, a.x, a.y, count);
    }
    if (kids)
        XFree(kids);
    return 0;
}

int main(int argc, char **argv)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy || argc < 2) {
        fprintf(stderr, "xtool: no display or no command\n");
        return 2;
    }
    if (strcmp(argv[1], "events") == 0)
        events(dpy);
    if (strcmp(argv[1], "own") == 0 && argc >= 3)
        own(dpy, argv[2], argc > 3 && strcmp(argv[3], "--secret") == 0);
    if (strcmp(argv[1], "windows") == 0) {
        int rc = windows(dpy);
        XCloseDisplay(dpy);
        return rc;
    }
    if (strcmp(argv[1], "pointer") == 0) {
        Window r, c;
        int rx, ry, wx, wy;
        unsigned m;
        XQueryPointer(dpy, DefaultRootWindow(dpy), &r, &c, &rx, &ry, &wx, &wy, &m);
        printf("pointer %d %d\n", rx, ry);
        XCloseDisplay(dpy);
        return 0;
    }
    return 2;
}
