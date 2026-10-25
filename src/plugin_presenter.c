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
 * The pointer is a small override-redirect window shaped with the XShape
 * extension (pixels with alpha under 128 are cut away), so it needs no
 * compositor and no ARGB visual, and its input region is empty: clicks go
 * through to what is under it. Like the upstream it starts in the middle of
 * the screen. It disappears 0.5 s after the last movement unless [pointer]
 * hide_after says otherwise. The screen is the whole X screen (the upstream
 * uses the window's own screen under Qt; with several monitors the pointer
 * ranges over all of them).
 *
 * Its look comes from the [pointer] section (the upstream's is a fixed red
 * dot): a built-in shape with a color and a rim, or a PNG or GIF (animated
 * too) that replaces them; see pointer_img.h. The frames are kept in the X
 * server between appearances and rebuilt when the settings change. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <X11/extensions/shape.h>

#include "config.h"
#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "pointer_img.h"
#include "util.h"
#include "x11.h"

#define PRESENTER "kdeconnect.presenter"
#define DEFAULT_HIDE_S 0.5
#define MAX_HIDE_S 3600

static const char *const incoming[] = {PRESENTER, NULL};

static struct {
    kr_x11 *x;
    Display *dpy;
    Window win;
    int w, h;          /* the window's size */
    double xpos, ypos; /* 0..1 */
    double hide_at;    /* 0: not scheduled */
    double hide_after; /* seconds; 0: only when the phone says stop */
    double next_frame; /* 0: not animating */
    int cur;
    bool source;
    /* The look, built from the settings: sig says from which. */
    char sig[2 * KR_PATH_MAX];
    int n;
    Pixmap *look, *mask;
    int *delay_ms;
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
    pr.next_frame = 0;
}

static void free_look(void)
{
    for (int i = 0; i < pr.n; i++) {
        XFreePixmap(pr.dpy, pr.look[i]);
        XFreePixmap(pr.dpy, pr.mask[i]);
    }
    free(pr.look);
    free(pr.mask);
    free(pr.delay_ms);
    pr.look = pr.mask = NULL;
    pr.delay_ms = NULL;
    pr.n = 0;
    pr.sig[0] = '\0';
}

/* An 8-bit component placed in the bits of a visual's channel mask. */
static unsigned long channel(unsigned long mask, unsigned v)
{
    if (!mask)
        return 0;
    int shift = __builtin_ctzl(mask), bits = __builtin_popcountl(mask >> shift);
    if (bits > 8)
        bits = 8;
    return (unsigned long)(v >> (8 - bits)) << shift;
}

/* Puts the frames onto server-side pixmaps: the colors, and a 1-bit mask of
 * where the pointer is. */
static bool upload(const kr_img *img)
{
    int scr = DefaultScreen(pr.dpy);
    Visual *vis = DefaultVisual(pr.dpy, scr);
    int depth = DefaultDepth(pr.dpy, scr);
    if (vis->class != TrueColor && vis->class != DirectColor) {
        kr_log_warn(_("the presentation pointer needs a true color display"));
        return false;
    }
    pr.look = calloc((size_t)img->n, sizeof *pr.look);
    pr.mask = calloc((size_t)img->n, sizeof *pr.mask);
    pr.delay_ms = calloc((size_t)img->n, sizeof *pr.delay_ms);
    if (!pr.look || !pr.mask || !pr.delay_ms)
        return false;
    Window root = DefaultRootWindow(pr.dpy);
    unsigned w = (unsigned)img->w, h = (unsigned)img->h;

    for (int f = 0; f < img->n; f++) {
        XImage *color = XCreateImage(pr.dpy, vis, (unsigned)depth, ZPixmap, 0, NULL, w, h, 32, 0);
        XImage *bits = XCreateImage(pr.dpy, vis, 1, XYBitmap, 0, NULL, w, h, 8, 0);
        if (color)
            color->data = calloc((size_t)color->bytes_per_line, h);
        if (bits)
            bits->data = calloc((size_t)bits->bytes_per_line, h);
        if (!color || !bits || !color->data || !bits->data) {
            if (color)
                XDestroyImage(color); /* frees the data if there is any */
            if (bits)
                XDestroyImage(bits);
            return false;
        }
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) {
                uint32_t p = img->px[f][(size_t)y * w + x];
                XPutPixel(bits, (int)x, (int)y, (p >> 24) >= 128);
                XPutPixel(color, (int)x, (int)y,
                          channel(vis->red_mask, (p >> 16) & 0xff) | channel(vis->green_mask, (p >> 8) & 0xff) |
                              channel(vis->blue_mask, p & 0xff));
            }
        pr.look[f] = XCreatePixmap(pr.dpy, root, w, h, (unsigned)depth);
        pr.mask[f] = XCreatePixmap(pr.dpy, root, w, h, 1);
        pr.n = f + 1; /* from here free_look() frees these two */
        GC gc = XCreateGC(pr.dpy, pr.look[f], 0, NULL);
        XPutImage(pr.dpy, pr.look[f], gc, color, 0, 0, 0, 0, w, h);
        XFreeGC(pr.dpy, gc);
        gc = XCreateGC(pr.dpy, pr.mask[f], 0, NULL);
        XSetForeground(pr.dpy, gc, 1);
        XSetBackground(pr.dpy, gc, 0);
        XPutImage(pr.dpy, pr.mask[f], gc, bits, 0, 0, 0, 0, w, h);
        XFreeGC(pr.dpy, gc);
        XDestroyImage(color);
        XDestroyImage(bits);
        pr.delay_ms[f] = img->delay_ms[f];
    }
    return true;
}

static kr_shape shape_named(const char *name)
{
    static const struct {
        const char *name;
        kr_shape shape;
    } shapes[] = {{"ring", KR_SHAPE_RING},   {"square", KR_SHAPE_SQUARE}, {"diamond", KR_SHAPE_DIAMOND},
                  {"cross", KR_SHAPE_CROSS}, {"circle", KR_SHAPE_CIRCLE}};
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++)
        if (name && strcmp(shapes[i].name, name) == 0)
            return shapes[i].shape;
    return KR_SHAPE_CIRCLE;
}

/* The biggest side of the pointer, as a share of the screen's width (the
 * old fixed size is "medium"). */
static int size_for(const char *name, int screen_w)
{
    static const struct {
        const char *name;
        int divisor;
    } sizes[] = {{"small", 300}, {"large", 100}, {"huge", 60}, {"medium", 150}};
    int div = 150;
    for (size_t i = 0; name && i < sizeof sizes / sizeof *sizes; i++)
        if (strcmp(sizes[i].name, name) == 0)
            div = sizes[i].divisor;
    int side = screen_w / div;
    return side < 12 ? 12 : side;
}

static const char *get(const kr_config *cfg, const char *key, const char *def)
{
    const char *v = kr_config_get(cfg, "pointer", key);
    return v && *v ? v : def;
}

/* Makes the look match the settings (nothing to do if it already does) and
 * reads hide_after. A bad image falls back to the shape, with a warning,
 * once per change of settings. */
static bool build_look(const kr_config *cfg)
{
    int scr = DefaultScreen(pr.dpy);
    int side = size_for(get(cfg, "size", "medium"), DisplayWidth(pr.dpy, scr));
    const char *shape_name = get(cfg, "shape", "circle");
    bool rim = kr_config_get_bool(cfg, "pointer", "outline", true);
    uint32_t color, rim_color;
    char image[KR_PATH_MAX] = "";
    long mtime = 0;

    char *end;
    double hs = strtod(get(cfg, "hide_after", "0.5"), &end);
    pr.hide_after = *end || !isfinite(hs) || hs < 0 ? DEFAULT_HIDE_S : hs > MAX_HIDE_S ? MAX_HIDE_S : hs;

    if (!kr_color_parse(get(cfg, "color", ""), &color))
        color = 0xff0000;
    if (!kr_color_parse(get(cfg, "outline_color", ""), &rim_color))
        rim_color = 0xffffff;
    if (kr_config_get_path(cfg, "pointer", "image", image, sizeof image)) {
        struct stat st;
        if (stat(image, &st) == 0)
            mtime = (long)st.st_mtime;
    }
    char sig[sizeof pr.sig];
    snprintf(sig, sizeof sig, "%d|%s|%06x|%d|%06x|%ld|%s", side, shape_name, (unsigned)color, rim, (unsigned)rim_color,
             mtime, image);
    if (pr.n && strcmp(sig, pr.sig) == 0)
        return true;

    free_look();
    kr_img img;
    char err[160];
    bool ok = false;
    if (image[0]) {
        ok = kr_img_load(image, side, &img, err, sizeof err);
        if (!ok)
            kr_log_warn(_("pointer image %s: %s; using the shape instead"), image, err);
    }
    if (!ok && !kr_img_shape(shape_named(shape_name), side, color, rim, rim_color, &img))
        return false;
    pr.w = img.w;
    pr.h = img.h;
    ok = upload(&img);
    kr_img_free(&img);
    if (!ok) {
        free_look();
        return false;
    }
    snprintf(pr.sig, sizeof pr.sig, "%s", sig);
    return true;
}

static void set_frame(int i)
{
    pr.cur = i;
    XSetWindowBackgroundPixmap(pr.dpy, pr.win, pr.look[i]);
    XShapeCombineMask(pr.dpy, pr.win, ShapeBounding, 0, 0, pr.mask[i], ShapeSet);
}

static void show(const kr_config *cfg)
{
    if (!build_look(cfg))
        return;
    pr.xpos = pr.ypos = 0.5;

    XSetWindowAttributes attrs = {.override_redirect = True};
    pr.win = XCreateWindow(pr.dpy, DefaultRootWindow(pr.dpy), 0, 0, (unsigned)pr.w, (unsigned)pr.h, 0, CopyFromParent,
                           InputOutput, CopyFromParent, CWOverrideRedirect, &attrs);
    set_frame(0);
    /* No input region: clicks pass through. */
    XShapeCombineRectangles(pr.dpy, pr.win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    XMapWindow(pr.dpy, pr.win);
    pr.next_frame = pr.n > 1 ? kr_now() + pr.delay_ms[0] / 1000.0 : 0;
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)dev;
    if (!pr.x)
        return;
    if (kr_packet_bool(p, "stop", false)) {
        hide();
        return;
    }
    if (!pr.win)
        show(kr_daemon_config(d));
    if (!pr.win)
        return;
    int scr = DefaultScreen(pr.dpy);
    int w = DisplayWidth(pr.dpy, scr), h = DisplayHeight(pr.dpy, scr);
    double ratio = (double)w / h;
    pr.xpos += num(p, "dx");
    pr.ypos += num(p, "dy") * ratio;
    pr.xpos = pr.xpos < 0 ? 0 : pr.xpos > 1 ? 1 : pr.xpos;
    pr.ypos = pr.ypos < 0 ? 0 : pr.ypos > 1 ? 1 : pr.ypos;
    XMoveWindow(pr.dpy, pr.win, (int)(pr.xpos * w) - pr.w / 2, (int)(pr.ypos * h) - pr.h / 2);
    XRaiseWindow(pr.dpy, pr.win);
    XFlush(pr.dpy);
    pr.hide_at = pr.hide_after > 0 ? kr_now() + pr.hide_after : 0;
}

static double src_deadline(void *ud)
{
    (void)ud;
    if (pr.hide_at && (!pr.next_frame || pr.hide_at < pr.next_frame))
        return pr.hide_at;
    return pr.next_frame;
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    (void)ud;
    (void)fds;
    (void)n;
    double now = kr_now();
    if (pr.hide_at && now >= pr.hide_at) {
        hide();
    } else if (pr.win && pr.next_frame && now >= pr.next_frame) {
        set_frame((pr.cur + 1) % pr.n);
        XClearWindow(pr.dpy, pr.win);
        XFlush(pr.dpy);
        pr.next_frame = now + pr.delay_ms[pr.cur] / 1000.0;
    }
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
    if (pr.x) {
        hide();
        free_look();
        XFlush(pr.dpy);
    }
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
