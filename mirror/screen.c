/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mirror's window (screen.h). Only 32-bit TrueColor visuals with 8 bits
 * per channel are drawn (what every desktop has); scaling and the YUV to
 * RGB conversion are libswscale's. */
#include "screen.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XShm.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "i18n.h"
#include "log.h"

struct kr_screen {
    Display *dpy;
    Window win;
    GC gc;
    Atom wm_delete;
    enum AVPixelFormat dst_fmt;
    unsigned win_w, win_h; /* the window now */
    unsigned vid_w, vid_h; /* the video's shape */
    /* The image the last frame was scaled into, and where it goes. */
    XImage *img;
    XShmSegmentInfo shm;
    bool use_shm, shm_on;
    unsigned img_w, img_h;
    int dst_x, dst_y;
    struct SwsContext *sws;
    AVFrame *last;
    XIM im;
    XIC ic;
    kr_screen_input in;
    void *in_ud;
    unsigned char held[256]; /* keycodes down, to tell auto-repeat */
    /* The clipboard. */
    Atom a_clipboard, a_utf8, a_targets, a_string, a_text, a_incr, a_prop, a_atom;
    int fixes_base; /* XFixes' event base, -1 without it */
    char *clip;     /* what we hold */
    size_t clip_len;
    bool clip_mine;
    void (*clip_changed)(void *ud);
    void *clip_changed_ud;
    kr_clip_cb clip_cb;
    void *clip_cb_ud;
};

static void free_image(kr_screen *s)
{
    if (!s->img)
        return;
    if (s->shm_on) {
        XShmDetach(s->dpy, &s->shm);
        XSync(s->dpy, False);
        s->img->data = NULL;
        XDestroyImage(s->img);
        shmdt(s->shm.shmaddr);
        s->shm_on = false;
    } else {
        free(s->img->data);
        s->img->data = NULL;
        XDestroyImage(s->img);
    }
    s->img = NULL;
}

static bool make_image(kr_screen *s, unsigned w, unsigned h)
{
    free_image(s);
    Visual *vis = DefaultVisual(s->dpy, DefaultScreen(s->dpy));
    int depth = DefaultDepth(s->dpy, DefaultScreen(s->dpy));
    if (s->use_shm) {
        s->img = XShmCreateImage(s->dpy, vis, (unsigned)depth, ZPixmap, NULL, &s->shm, w, h);
        if (s->img) {
            s->shm.shmid = shmget(IPC_PRIVATE, (size_t)s->img->bytes_per_line * h, IPC_CREAT | 0600);
            s->shm.shmaddr = s->shm.shmid >= 0 ? shmat(s->shm.shmid, NULL, 0) : (char *)-1;
            if (s->shm.shmaddr != (char *)-1) {
                s->img->data = s->shm.shmaddr;
                s->shm.readOnly = False;
                XShmAttach(s->dpy, &s->shm);
                XSync(s->dpy, False);
                shmctl(s->shm.shmid, IPC_RMID, NULL); /* gone once both detach */
                s->shm_on = true;
            } else {
                if (s->shm.shmid >= 0)
                    shmctl(s->shm.shmid, IPC_RMID, NULL);
                XDestroyImage(s->img);
                s->img = NULL;
                s->use_shm = false;
                kr_log_debug("screen: no shared memory, plain XPutImage");
            }
        } else {
            s->use_shm = false;
        }
    }
    if (!s->img) {
        s->img = XCreateImage(s->dpy, vis, (unsigned)depth, ZPixmap, 0, NULL, w, h, 32, 0);
        if (!s->img)
            return false;
        s->img->data = malloc((size_t)s->img->bytes_per_line * h);
        if (!s->img->data) {
            XDestroyImage(s->img);
            s->img = NULL;
            return false;
        }
    }
    s->img_w = w;
    s->img_h = h;
    return true;
}

/* w x h, as big as fits in box_w x box_h with the same shape. */
static void fit(unsigned w, unsigned h, unsigned box_w, unsigned box_h, unsigned *out_w, unsigned *out_h)
{
    if ((unsigned long long)box_w * h <= (unsigned long long)box_h * w) {
        *out_w = box_w;
        *out_h = (unsigned)((unsigned long long)box_w * h / w);
    } else {
        *out_h = box_h;
        *out_w = (unsigned)((unsigned long long)box_h * w / h);
    }
    if (!*out_w)
        *out_w = 1;
    if (!*out_h)
        *out_h = 1;
}

static void set_hints(kr_screen *s)
{
    XSizeHints *h = XAllocSizeHints();
    if (!h)
        return;
    h->flags = PMinSize | PAspect;
    h->min_width = 64;
    h->min_height = 64;
    h->min_aspect.x = h->max_aspect.x = (int)s->vid_w;
    h->min_aspect.y = h->max_aspect.y = (int)s->vid_h;
    XSetWMNormalHints(s->dpy, s->win, h);
    XFree(h);
}

/* The window for the video: the video's size, shrunk to 90% of the screen. */
static void initial_size(kr_screen *s, unsigned *w, unsigned *h)
{
    Screen *scr = DefaultScreenOfDisplay(s->dpy);
    unsigned max_w = (unsigned)WidthOfScreen(scr) * 9 / 10, max_h = (unsigned)HeightOfScreen(scr) * 9 / 10;
    if (s->vid_w <= max_w && s->vid_h <= max_h) {
        *w = s->vid_w;
        *h = s->vid_h;
    } else {
        fit(s->vid_w, s->vid_h, max_w, max_h, w, h);
    }
}

void kr_screen_set_title(kr_screen *s, const char *title)
{
    Atom utf8 = XInternAtom(s->dpy, "UTF8_STRING", False);
    Atom net_name = XInternAtom(s->dpy, "_NET_WM_NAME", False);
    XChangeProperty(s->dpy, s->win, net_name, utf8, 8, PropModeReplace, (const unsigned char *)title,
                    (int)strlen(title));
    XStoreName(s->dpy, s->win, title); /* WM_NAME: for window managers without _NET_WM_NAME */
    XFlush(s->dpy);
}

kr_screen *kr_screen_new(const char *title, unsigned width, unsigned height, const char **why)
{
    kr_screen *s = calloc(1, sizeof *s);
    if (!s) {
        *why = _("Out of memory");
        return NULL;
    }
    s->dpy = XOpenDisplay(NULL);
    if (!s->dpy) {
        *why = _("Could not open the X display");
        free(s);
        return NULL;
    }
    int scr = DefaultScreen(s->dpy);
    Visual *vis = DefaultVisual(s->dpy, scr);
    XVisualInfo tmpl = {.visualid = XVisualIDFromVisual(vis)};
    int nvi = 0;
    XVisualInfo *vi = XGetVisualInfo(s->dpy, VisualIDMask, &tmpl, &nvi);
    int bpp = 0;
    int nf = 0;
    XPixmapFormatValues *pf = XListPixmapFormats(s->dpy, &nf);
    for (int i = 0; pf && i < nf; i++)
        if (pf[i].depth == DefaultDepth(s->dpy, scr))
            bpp = pf[i].bits_per_pixel;
    if (pf)
        XFree(pf);
    bool lsb = ImageByteOrder(s->dpy) == LSBFirst;
    s->dst_fmt = AV_PIX_FMT_NONE;
    if (vi && vi->class == TrueColor && bpp == 32) {
        /* 0x00RRGGBB in a native 32-bit word. */
        if (vi->red_mask == 0xff0000 && vi->green_mask == 0xff00 && vi->blue_mask == 0xff)
            s->dst_fmt = lsb ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_0RGB;
        else if (vi->red_mask == 0xff && vi->green_mask == 0xff00 && vi->blue_mask == 0xff0000)
            s->dst_fmt = lsb ? AV_PIX_FMT_RGB0 : AV_PIX_FMT_0BGR;
    }
    if (vi)
        XFree(vi);
    if (s->dst_fmt == AV_PIX_FMT_NONE) {
        *why = _("The X display needs 24-bit color for the mirror");
        XCloseDisplay(s->dpy);
        free(s);
        return NULL;
    }
    int major, minor;
    Bool pixmaps;
    const char *name = DisplayString(s->dpy);
    s->use_shm = XShmQueryVersion(s->dpy, &major, &minor, &pixmaps) && name && name[0] == ':';

    s->vid_w = width;
    s->vid_h = height;
    initial_size(s, &s->win_w, &s->win_h);
    XSetWindowAttributes a = {.background_pixel = BlackPixel(s->dpy, scr),
                              .event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                                            ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                                            FocusChangeMask};
    s->win = XCreateWindow(s->dpy, RootWindow(s->dpy, scr), 0, 0, s->win_w, s->win_h, 0, CopyFromParent,
                           InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &a);
    s->gc = XCreateGC(s->dpy, s->win, 0, NULL);

    XClassHint *ch = XAllocClassHint();
    if (ch) {
        ch->res_name = (char *)"kiremote-mirror";
        ch->res_class = (char *)"Kiremote-mirror";
        XSetClassHint(s->dpy, s->win, ch);
        XFree(ch);
    }
    kr_screen_set_title(s, title);
    s->wm_delete = XInternAtom(s->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s->dpy, s->win, &s->wm_delete, 1);
    set_hints(s);

    /* Auto-repeat as presses only (no release in between), and an input
     * method for the text keys type (dead keys, compose). */
    XkbSetDetectableAutoRepeat(s->dpy, True, NULL);
    XSetLocaleModifiers("");
    s->im = XOpenIM(s->dpy, NULL, NULL, NULL);
    if (s->im)
        s->ic = XCreateIC(s->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, s->win,
                          XNFocusWindow, s->win, NULL);
    if (!s->ic)
        kr_log_debug("screen: no input method, keys only as X looks them up");

    s->a_clipboard = XInternAtom(s->dpy, "CLIPBOARD", False);
    s->a_utf8 = XInternAtom(s->dpy, "UTF8_STRING", False);
    s->a_targets = XInternAtom(s->dpy, "TARGETS", False);
    s->a_string = XA_STRING;
    s->a_text = XInternAtom(s->dpy, "TEXT", False);
    s->a_incr = XInternAtom(s->dpy, "INCR", False);
    s->a_prop = XInternAtom(s->dpy, "KIREMOTE_CLIPBOARD", False);
    s->a_atom = XA_ATOM;
    int fixes_error;
    if (XFixesQueryExtension(s->dpy, &s->fixes_base, &fixes_error))
        XFixesSelectSelectionInput(s->dpy, s->win, s->a_clipboard, XFixesSetSelectionOwnerNotifyMask);
    else
        s->fixes_base = -1;

    XMapWindow(s->dpy, s->win);
    XFlush(s->dpy);
    return s;
}

void kr_screen_set_input(kr_screen *s, const kr_screen_input *cbs, void *ud)
{
    s->in = *cbs;
    s->in_ud = ud;
}

void kr_screen_raise(kr_screen *s)
{
    XMapRaised(s->dpy, s->win);
    XEvent ev = {.xclient = {.type = ClientMessage,
                             .window = s->win,
                             .message_type = XInternAtom(s->dpy, "_NET_ACTIVE_WINDOW", False),
                             .format = 32,
                             .data.l = {1 /* from an application */, CurrentTime, 0, 0, 0}}};
    XSendEvent(s->dpy, DefaultRootWindow(s->dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(s->dpy);
}

void kr_screen_toggle_fullscreen(kr_screen *s)
{
    Atom state = XInternAtom(s->dpy, "_NET_WM_STATE", False);
    XEvent ev = {.xclient = {.type = ClientMessage,
                             .window = s->win,
                             .message_type = state,
                             .format = 32,
                             .data.l = {2 /* toggle */, (long)XInternAtom(s->dpy, "_NET_WM_STATE_FULLSCREEN", False),
                                        0, 1, 0}}};
    XSendEvent(s->dpy, DefaultRootWindow(s->dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(s->dpy);
}

void kr_screen_free(kr_screen *s)
{
    if (!s)
        return;
    free_image(s);
    free(s->clip);
    if (s->ic)
        XDestroyIC(s->ic);
    if (s->im)
        XCloseIM(s->im);
    sws_freeContext(s->sws);
    av_frame_free(&s->last);
    XFreeGC(s->dpy, s->gc);
    XDestroyWindow(s->dpy, s->win);
    XCloseDisplay(s->dpy);
    free(s);
}

int kr_screen_fd(const kr_screen *s)
{
    return ConnectionNumber(s->dpy);
}

/* Scales s->last into the window and puts it there. */
static void draw(kr_screen *s, bool scale)
{
    const AVFrame *f = s->last;
    if (!f || f->width <= 0 || f->height <= 0)
        return;
    unsigned dw, dh;
    fit((unsigned)f->width, (unsigned)f->height, s->win_w, s->win_h, &dw, &dh);
    int dx = (int)(s->win_w - dw) / 2, dy = (int)(s->win_h - dh) / 2;
    if (!s->img || s->img_w != dw || s->img_h != dh) {
        if (!make_image(s, dw, dh))
            return;
        scale = true;
    }
    if (dx != s->dst_x || dy != s->dst_y) {
        XClearWindow(s->dpy, s->win); /* the bars, in black */
        s->dst_x = dx;
        s->dst_y = dy;
    }
    if (scale) {
        s->sws = sws_getCachedContext(s->sws, f->width, f->height, (enum AVPixelFormat)f->format, (int)dw, (int)dh,
                                      s->dst_fmt, SWS_BILINEAR, NULL, NULL, NULL);
        if (!s->sws)
            return;
        uint8_t *dst[4] = {(uint8_t *)s->img->data, NULL, NULL, NULL};
        int stride[4] = {s->img->bytes_per_line, 0, 0, 0};
        sws_scale(s->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride);
    }
    if (s->shm_on)
        XShmPutImage(s->dpy, s->win, s->gc, s->img, 0, 0, dx, dy, dw, dh, False);
    else
        XPutImage(s->dpy, s->win, s->gc, s->img, 0, 0, dx, dy, dw, dh);
    XFlush(s->dpy);
}

void kr_screen_frame(kr_screen *s, const AVFrame *frame)
{
    if (!s->last)
        s->last = av_frame_alloc();
    if (!s->last)
        return;
    av_frame_unref(s->last);
    if (av_frame_ref(s->last, frame) < 0)
        return;
    draw(s, true);
}

void kr_screen_video_size(kr_screen *s, unsigned width, unsigned height)
{
    if (width == s->vid_w && height == s->vid_h)
        return;
    /* Same longer side as now, the new shape (a rotation swaps them). */
    unsigned longer = s->win_w > s->win_h ? s->win_w : s->win_h;
    s->vid_w = width;
    s->vid_h = height;
    unsigned w, h;
    fit(width, height, longer, longer, &w, &h);
    unsigned max_w, max_h;
    initial_size(s, &max_w, &max_h);
    if (w > max_w || h > max_h)
        fit(width, height, max_w, max_h, &w, &h);
    set_hints(s);
    XResizeWindow(s->dpy, s->win, w, h);
    XFlush(s->dpy);
}

/* A window position to the video's pixels, clamped to the picture. */
static void to_video(const kr_screen *s, int wx, int wy, int *x, int *y)
{
    int iw = s->img ? (int)s->img_w : (int)s->win_w, ih = s->img ? (int)s->img_h : (int)s->win_h;
    int px = wx - (s->img ? s->dst_x : 0), py = wy - (s->img ? s->dst_y : 0);
    if (px < 0)
        px = 0;
    if (py < 0)
        py = 0;
    if (px >= iw)
        px = iw - 1;
    if (py >= ih)
        py = ih - 1;
    *x = (int)((long long)px * s->vid_w / (iw > 0 ? iw : 1));
    *y = (int)((long long)py * s->vid_h / (ih > 0 ? ih : 1));
}

static void key_event(kr_screen *s, XKeyEvent *k)
{
    if (!s->in.on_key)
        return;
    bool down = k->type == KeyPress;
    unsigned char *held = &s->held[k->keycode & 0xff];
    bool repeat = down && *held;
    *held = down;
    char text[64] = "";
    KeySym sym = NoSymbol;
    if (down && s->ic) {
        Status st;
        int n = Xutf8LookupString(s->ic, k, text, sizeof text - 1, &sym, &st);
        if (st == XBufferOverflow || n < 0)
            n = 0;
        text[n] = 0;
        if (st != XLookupKeySym && st != XLookupBoth)
            sym = NoSymbol;
        if (st != XLookupChars && st != XLookupBoth)
            text[0] = 0;
    } else {
        int n = XLookupString(k, text, sizeof text - 1, &sym, NULL);
        text[n > 0 ? n : 0] = 0;
        if (!down || (unsigned char)text[0] >= 0x80)
            text[0] = 0; /* Latin-1 from XLookupString: only ASCII is safe as UTF-8 */
    }
    /* The keysym without the modifiers' effect, so letters are found by
     * their key whatever Shift or Caps do (and on release, when the input
     * method says nothing). */
    KeySym base = XLookupKeysym(k, 0);
    if (sym == NoSymbol && !down)
        XLookupString(k, NULL, 0, &sym, NULL);
    /* The keypad is the other way round: Num Lock decides what it is. */
    KeySym use = (IsKeypadKey(base) && sym != NoSymbol) || base == NoSymbol ? sym : base;
    s->in.on_key(s->in_ud, use, down, repeat, k->state, text);
}

/* ---- clipboard --------------------------------------------------------------- */

void kr_screen_clipboard_watch(kr_screen *s, void (*on_changed)(void *ud), void *ud)
{
    s->clip_changed = on_changed;
    s->clip_changed_ud = ud;
}

void kr_screen_clipboard_get(kr_screen *s, kr_clip_cb cb, void *ud)
{
    s->clip_cb = cb;
    s->clip_cb_ud = ud;
    XConvertSelection(s->dpy, s->a_clipboard, s->a_utf8, s->a_prop, s->win, CurrentTime);
    XFlush(s->dpy);
}

void kr_screen_clipboard_set(kr_screen *s, const char *text, size_t len)
{
    /* One property is one request; X's limit is in 4-byte units. */
    if (len > (size_t)XMaxRequestSize(s->dpy) * 4 - 1024) {
        kr_log_debug("screen: clipboard of %zu bytes is too big for X", len);
        return;
    }
    char *copy = malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, text, len);
    copy[len] = 0;
    free(s->clip);
    s->clip = copy;
    s->clip_len = len;
    XSetSelectionOwner(s->dpy, s->a_clipboard, s->win, CurrentTime);
    s->clip_mine = XGetSelectionOwner(s->dpy, s->a_clipboard) == s->win;
    XFlush(s->dpy);
}

bool kr_screen_clipboard_mine(const kr_screen *s)
{
    return s->clip_mine;
}

/* Somebody asks us for the text we hold. */
static void clip_request(kr_screen *s, const XSelectionRequestEvent *rq)
{
    XEvent ev = {.xselection = {.type = SelectionNotify,
                                .display = rq->display,
                                .requestor = rq->requestor,
                                .selection = rq->selection,
                                .target = rq->target,
                                .property = None,
                                .time = rq->time}};
    Atom prop = rq->property != None ? rq->property : rq->target; /* old clients */
    if (s->clip_mine && rq->selection == s->a_clipboard) {
        if (rq->target == s->a_targets) {
            Atom list[] = {s->a_targets, s->a_utf8, s->a_string, s->a_text};
            XChangeProperty(s->dpy, rq->requestor, prop, s->a_atom, 32, PropModeReplace, (unsigned char *)list,
                            (int)(sizeof list / sizeof *list));
            ev.xselection.property = prop;
        } else if (rq->target == s->a_utf8 || rq->target == s->a_string || rq->target == s->a_text) {
            XChangeProperty(s->dpy, rq->requestor, prop, rq->target == s->a_string ? s->a_string : s->a_utf8, 8,
                            PropModeReplace, (unsigned char *)s->clip, (int)s->clip_len);
            ev.xselection.property = prop;
        }
    }
    XSendEvent(s->dpy, rq->requestor, False, NoEventMask, &ev);
    XFlush(s->dpy);
}

/* The answer to kr_screen_clipboard_get. */
static void clip_answer(kr_screen *s, const XSelectionEvent *se)
{
    kr_clip_cb cb = s->clip_cb;
    void *ud = s->clip_cb_ud;
    s->clip_cb = NULL;
    if (!cb)
        return;
    if (se->property == None || se->selection != s->a_clipboard) {
        cb(ud, NULL, 0);
        return;
    }
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    int rc = XGetWindowProperty(s->dpy, s->win, s->a_prop, 0, (long)(KR_CLIP_MAX / 4 + 1), True, AnyPropertyType, &type,
                                &format, &n, &after, &data);
    if (rc == Success && type == s->a_utf8 && format == 8 && after == 0 && data)
        cb(ud, (const char *)data, n);
    else
        cb(ud, NULL, 0); /* INCR, too long or not text */
    if (data)
        XFree(data);
}

bool kr_screen_dispatch(kr_screen *s)
{
    bool open = true;
    while (XPending(s->dpy)) {
        XEvent ev;
        XNextEvent(s->dpy, &ev);
        if (XFilterEvent(&ev, None))
            continue; /* the input method took it (a dead key, compose) */
        int x, y;
        switch (ev.type) {
        case KeyPress:
        case KeyRelease:
            key_event(s, &ev.xkey);
            break;
        case ButtonPress:
        case ButtonRelease:
            if (s->in.on_button) {
                to_video(s, ev.xbutton.x, ev.xbutton.y, &x, &y);
                s->in.on_button(s->in_ud, (int)ev.xbutton.button, ev.type == ButtonPress, x, y, ev.xbutton.state);
            }
            break;
        case MotionNotify:
            /* Only the latest of a burst of motions. */
            while (XCheckTypedWindowEvent(s->dpy, s->win, MotionNotify, &ev))
                ;
            if (s->in.on_motion) {
                to_video(s, ev.xmotion.x, ev.xmotion.y, &x, &y);
                s->in.on_motion(s->in_ud, x, y, ev.xmotion.state);
            }
            break;
        case FocusIn:
            if (s->ic)
                XSetICFocus(s->ic);
            break;
        case SelectionRequest:
            clip_request(s, &ev.xselectionrequest);
            break;
        case SelectionClear:
            if (ev.xselectionclear.selection == s->a_clipboard) {
                s->clip_mine = false;
                free(s->clip);
                s->clip = NULL;
                s->clip_len = 0;
            }
            break;
        case SelectionNotify:
            clip_answer(s, &ev.xselection);
            break;
        case FocusOut:
            if (s->ic)
                XUnsetICFocus(s->ic);
            memset(s->held, 0, sizeof s->held);
            if (s->in.on_focus_out)
                s->in.on_focus_out(s->in_ud);
            break;
        case ConfigureNotify:
            if ((unsigned)ev.xconfigure.width != s->win_w || (unsigned)ev.xconfigure.height != s->win_h) {
                s->win_w = (unsigned)ev.xconfigure.width;
                s->win_h = (unsigned)ev.xconfigure.height;
                s->dst_x = s->dst_y = -1; /* clear the bars on the next draw */
                draw(s, true);
            }
            break;
        case Expose:
            if (ev.xexpose.count == 0) {
                s->dst_x = s->dst_y = -1;
                draw(s, false);
            }
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == s->wm_delete)
                open = false;
            break;
        default:
            if (s->fixes_base >= 0 && ev.type == s->fixes_base + XFixesSelectionNotify) {
                const XFixesSelectionNotifyEvent *fe = (const XFixesSelectionNotifyEvent *)&ev;
                if (fe->selection == s->a_clipboard && fe->owner != s->win) {
                    s->clip_mine = false; /* it is not ours any more */
                    if (s->clip_changed)
                        s->clip_changed(s->clip_changed_ud);
                }
            }
            break;
        }
    }
    return open;
}
