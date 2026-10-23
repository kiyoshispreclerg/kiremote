/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mirror's window (screen.h). Only 32-bit TrueColor visuals with 8 bits
 * per channel are drawn (what every desktop has); scaling and the YUV to
 * RGB conversion are libswscale's. */
#include "screen.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
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
                              .event_mask = ExposureMask | StructureNotifyMask | KeyPressMask};
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
    Atom utf8 = XInternAtom(s->dpy, "UTF8_STRING", False);
    Atom net_name = XInternAtom(s->dpy, "_NET_WM_NAME", False);
    XChangeProperty(s->dpy, s->win, net_name, utf8, 8, PropModeReplace, (const unsigned char *)title,
                    (int)strlen(title));
    XStoreName(s->dpy, s->win, title); /* WM_NAME: for window managers without _NET_WM_NAME */
    s->wm_delete = XInternAtom(s->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s->dpy, s->win, &s->wm_delete, 1);
    set_hints(s);
    XMapWindow(s->dpy, s->win);
    XFlush(s->dpy);
    return s;
}

void kr_screen_free(kr_screen *s)
{
    if (!s)
        return;
    free_image(s);
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

bool kr_screen_dispatch(kr_screen *s)
{
    bool open = true;
    while (XPending(s->dpy)) {
        XEvent ev;
        XNextEvent(s->dpy, &ev);
        switch (ev.type) {
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
            break;
        }
    }
    return open;
}
