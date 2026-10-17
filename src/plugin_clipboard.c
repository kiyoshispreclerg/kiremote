/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.clipboard, both ways (plugins/clipboard upstream; and/plugins/
 * clipboard for the phone):
 *
 *   {"content": "text"}                         the clipboard changed
 *   {"content": "text", "timestamp": ms}        sent on connection:
 *        the newer clipboard wins (a timestamp of 0, or older than ours, is
 *        ignored)
 *
 * The upstream also has kdeconnect.clipboard.file (a file or image as a
 * payload); the Android app doesn't send or take it, so it is not here.
 *
 * On X11, with no toolkit: XFixes tells us when someone else takes the
 * CLIPBOARD selection; we then ask for its TARGETS and read UTF8_STRING
 * (or STRING); and to put the phone's text there we own the selection
 * ourselves and answer the requests for it (TARGETS and the text targets;
 * no INCR, so texts that don't fit one request are not offered).
 *
 *   [clipboard]
 *   auto_share = true        # send what is copied here to the phone
 *   share_passwords = false  # even what a password manager marks as secret
 *                            # (x-kde-passwordManagerHint; the upstream
 *                            # sends it by default) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include "log.h"
#include "plugin.h"
#include "util.h"
#include "x11.h"

#define CLIPBOARD "kdeconnect.clipboard"
#define CONNECT "kdeconnect.clipboard.connect"
#define MAX_TEXT (8u << 20) /* what we read from another program */
#define READ_TIMEOUT_S 1.5

static const char *const types[] = {CLIPBOARD, CONNECT, NULL};

static struct {
    kr_x11 *x;
    Display *dpy;
    Window win;
    int xfixes_event;
    Atom clipboard, utf8, targets, text, mime_utf8, mime_plain, incr, secret, prop;

    char *content;       /* the clipboard as we last knew it */
    int64_t stamp_ms;    /* when it changed (the upstream's updateTimestamp) */
    bool secret_content; /* a password manager put it there */

    char *own;           /* text we offer as the selection owner */
    size_t own_len;

    enum { RD_IDLE, RD_TARGETS, RD_TEXT } rd;
    Atom rd_target;
    bool rd_secret;
    double rd_deadline;
    bool source;
} cb;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- reading what others copy ------------------------------------------------ */

/* The property `prop` of our window as bytes; type, format and n describe it. NULL if
 * absent or too large (then it is deleted). Caller XFrees. */
static unsigned char *read_prop(Atom prop, Atom *type, int *format, unsigned long *n)
{
    unsigned char *data = NULL;
    unsigned long after = 0;
    if (XGetWindowProperty(cb.dpy, cb.win, prop, 0, (long)(MAX_TEXT / 4), True, AnyPropertyType, type, format, n,
                           &after, &data) != Success)
        return NULL;
    if (after > 0 || *type == None) {
        XFree(data);
        XDeleteProperty(cb.dpy, cb.win, prop);
        return NULL;
    }
    return data;
}

static void share_to_devices(kr_daemon *d, const char *text)
{
    kr_packet *p = kr_packet_new(CLIPBOARD);
    if (!p)
        return;
    cJSON_AddStringToObject(p->body, "content", text);
    for (int i = 0; i < kr_daemon_device_count(d); i++)
        kr_daemon_send(d, kr_daemon_device_at(d, i), p);
    kr_packet_free(p);
}

/* The clipboard changed under someone else's ownership. */
static void local_changed(kr_daemon *d, char *text, bool secret)
{
    if (!text[0] || (cb.content && strcmp(cb.content, text) == 0 && secret == cb.secret_content)) {
        free(text);
        return;
    }
    free(cb.content);
    cb.content = text;
    cb.stamp_ms = now_ms();
    cb.secret_content = secret;
    const kr_config *cfg = kr_daemon_config(d);
    if (!kr_config_get_bool(cfg, "clipboard", "auto_share", true))
        return;
    if (secret && !kr_config_get_bool(cfg, "clipboard", "share_passwords", false)) {
        kr_log_debug("clipboard: not sharing what a password manager marked as secret");
        return;
    }
    share_to_devices(d, text);
}

static void start_read(void)
{
    if (XGetSelectionOwner(cb.dpy, cb.clipboard) == None || XGetSelectionOwner(cb.dpy, cb.clipboard) == cb.win) {
        cb.rd = RD_IDLE;
        return;
    }
    cb.rd = RD_TARGETS;
    cb.rd_deadline = kr_now() + READ_TIMEOUT_S;
    XConvertSelection(cb.dpy, cb.clipboard, cb.targets, cb.prop, cb.win, CurrentTime);
    XFlush(cb.dpy);
}

/* The TARGETS answer: is there text, is it marked secret; ask for it. */
static void got_targets(void)
{
    Atom type;
    int format;
    unsigned long n;
    unsigned char *data = read_prop(cb.prop, &type, &format, &n);
    bool utf8 = false, str = false;
    cb.rd_secret = false;
    if (data && type == XA_ATOM && format == 32) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) {
            utf8 |= atoms[i] == cb.utf8;
            str |= atoms[i] == XA_STRING;
            cb.rd_secret |= atoms[i] == cb.secret;
        }
    }
    if (data)
        XFree(data);
    if (!utf8 && !str) {
        cb.rd = RD_IDLE; /* an image, files...: nothing we share */
        return;
    }
    cb.rd_target = utf8 ? cb.utf8 : XA_STRING;
    cb.rd = RD_TEXT;
    cb.rd_deadline = kr_now() + READ_TIMEOUT_S;
    XConvertSelection(cb.dpy, cb.clipboard, cb.rd_target, cb.prop, cb.win, CurrentTime);
    XFlush(cb.dpy);
}

static void got_text(kr_daemon *d)
{
    Atom type;
    int format;
    unsigned long n;
    unsigned char *data = read_prop(cb.prop, &type, &format, &n);
    cb.rd = RD_IDLE;
    if (!data || type == cb.incr || format != 8) {
        if (data)
            XFree(data);
        if (type == cb.incr)
            kr_log_debug("clipboard: text too large to share");
        return;
    }
    char *text = NULL;
    if (cb.rd_target == XA_STRING)
        text = kr_latin1_to_utf8((const char *)data, n);
    else if (kr_utf8_valid((const char *)data, n)) {
        text = malloc(n + 1);
        if (text) {
            memcpy(text, data, n);
            text[n] = '\0';
        }
    }
    XFree(data);
    if (text && strlen(text) == n)
        local_changed(d, text, cb.rd_secret);
    else
        free(text); /* not valid text, or with NULs in it */
}

/* ---- serving our own text -------------------------------------------------------- */

static void send_notify(XSelectionRequestEvent *rq, Atom property)
{
    XSelectionEvent ev = {.type = SelectionNotify,
                          .display = rq->display,
                          .requestor = rq->requestor,
                          .selection = rq->selection,
                          .target = rq->target,
                          .property = property,
                          .time = rq->time};
    XSendEvent(cb.dpy, rq->requestor, False, NoEventMask, (XEvent *)&ev);
    XFlush(cb.dpy);
}

static void answer_request(XSelectionRequestEvent *rq)
{
    Atom prop = rq->property != None ? rq->property : rq->target;
    if (rq->selection != cb.clipboard || !cb.own) {
        send_notify(rq, None);
        return;
    }
    if (rq->target == cb.targets) {
        Atom list[] = {cb.targets, cb.utf8, XA_STRING, cb.text, cb.mime_utf8, cb.mime_plain};
        XChangeProperty(cb.dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)list,
                        (int)(sizeof list / sizeof *list));
        send_notify(rq, prop);
        return;
    }
    long limit = (XExtendedMaxRequestSize(cb.dpy) ? XExtendedMaxRequestSize(cb.dpy) : XMaxRequestSize(cb.dpy)) * 4 - 1024;
    if (rq->target == cb.utf8 || rq->target == cb.text || rq->target == cb.mime_utf8 || rq->target == cb.mime_plain) {
        if ((long)cb.own_len > limit) {
            send_notify(rq, None);
            return;
        }
        XChangeProperty(cb.dpy, rq->requestor, prop, rq->target == cb.text ? cb.utf8 : rq->target, 8, PropModeReplace,
                        (unsigned char *)cb.own, (int)cb.own_len);
        send_notify(rq, prop);
        return;
    }
    if (rq->target == XA_STRING) {
        /* Latin-1: what doesn't fit becomes '?'. */
        char *l1 = malloc(cb.own_len + 1);
        size_t o = 0;
        for (size_t i = 0; l1 && i < cb.own_len;) {
            unsigned char c = (unsigned char)cb.own[i];
            if (c < 0x80) {
                l1[o++] = (char)c;
                i++;
            } else if ((c == 0xC2 || c == 0xC3) && i + 1 < cb.own_len) {
                l1[o++] = (char)(((c & 0x03) << 6) | ((unsigned char)cb.own[i + 1] & 0x3F));
                i += 2;
            } else {
                l1[o++] = '?';
                i++;
                while (i < cb.own_len && ((unsigned char)cb.own[i] & 0xC0) == 0x80)
                    i++;
            }
        }
        if (l1 && (long)o <= limit) {
            XChangeProperty(cb.dpy, rq->requestor, prop, XA_STRING, 8, PropModeReplace, (unsigned char *)l1, (int)o);
            send_notify(rq, prop);
        } else {
            send_notify(rq, None);
        }
        free(l1);
        return;
    }
    send_notify(rq, None);
}

/* The text the phone sent becomes the clipboard. */
bool kr_clipboard_set_text(kr_daemon *d, const char *text)
{
    (void)d;
    if (!cb.x || !text)
        return false;
    char *copy = strdup(text);
    if (!copy)
        return false;
    free(cb.own);
    cb.own = copy;
    cb.own_len = strlen(copy);
    free(cb.content);
    cb.content = strdup(text);
    cb.stamp_ms = now_ms();
    cb.secret_content = false;
    XSetSelectionOwner(cb.dpy, cb.clipboard, cb.win, CurrentTime);
    XFlush(cb.dpy);
    if (XGetSelectionOwner(cb.dpy, cb.clipboard) != cb.win) {
        kr_log_warn("cannot take the clipboard");
        return false;
    }
    return true;
}

/* ---- X events and timeouts ------------------------------------------------------------ */

static bool on_x_event(void *ud, XEvent *ev)
{
    kr_daemon *d = ud;
    if (ev->type == cb.xfixes_event + XFixesSelectionNotify) {
        XFixesSelectionNotifyEvent *sn = (XFixesSelectionNotifyEvent *)ev;
        if (sn->selection != cb.clipboard)
            return false;
        if (sn->owner != cb.win && sn->owner != None)
            start_read();
        else if (sn->owner != cb.win)
            cb.rd = RD_IDLE;
        return true;
    }
    switch (ev->type) {
    case SelectionNotify:
        if (ev->xselection.requestor != cb.win || ev->xselection.selection != cb.clipboard)
            return false;
        if (ev->xselection.property == None) {
            cb.rd = RD_IDLE; /* the owner refused or has nothing */
        } else if (cb.rd == RD_TARGETS) {
            got_targets();
        } else if (cb.rd == RD_TEXT) {
            got_text(d);
        }
        return true;
    case SelectionRequest:
        answer_request(&ev->xselectionrequest);
        return true;
    case SelectionClear:
        if (ev->xselectionclear.selection != cb.clipboard)
            return false;
        free(cb.own);
        cb.own = NULL;
        return true;
    default:
        return false;
    }
}

static double src_deadline(void *ud)
{
    (void)ud;
    return cb.rd != RD_IDLE ? cb.rd_deadline : 0;
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    (void)ud;
    (void)fds;
    (void)n;
    if (cb.rd != RD_IDLE && kr_now() >= cb.rd_deadline) {
        kr_log_debug("clipboard: the owner did not answer");
        cb.rd = RD_IDLE;
    }
}

static void init(kr_daemon *d)
{
    cb.x = kr_daemon_x11(d);
    if (!cb.x)
        return;
    cb.dpy = kr_x11_display(cb.x);
    cb.win = kr_x11_window(cb.x);
    int err;
    if (!XFixesQueryExtension(cb.dpy, &cb.xfixes_event, &err)) {
        kr_log_warn("the X server has no XFixes: the clipboard is not shared");
        cb.x = NULL;
        return;
    }
    cb.clipboard = XInternAtom(cb.dpy, "CLIPBOARD", False);
    cb.utf8 = XInternAtom(cb.dpy, "UTF8_STRING", False);
    cb.targets = XInternAtom(cb.dpy, "TARGETS", False);
    cb.text = XInternAtom(cb.dpy, "TEXT", False);
    cb.mime_utf8 = XInternAtom(cb.dpy, "text/plain;charset=utf-8", False);
    cb.mime_plain = XInternAtom(cb.dpy, "text/plain", False);
    cb.incr = XInternAtom(cb.dpy, "INCR", False);
    cb.secret = XInternAtom(cb.dpy, "x-kde-passwordManagerHint", False);
    cb.prop = XInternAtom(cb.dpy, "KIREMOTE_SELECTION", False);
    cb.stamp_ms = now_ms();
    XFixesSelectSelectionInput(cb.dpy, kr_x11_window(cb.x), cb.clipboard, XFixesSetSelectionOwnerNotifyMask);
    kr_x11_add_handler(cb.x, on_x_event, d);
    kr_daemon_add_source(d, &(kr_source){NULL, src_dispatch, src_deadline}, &cb);
    cb.source = true;
    start_read(); /* what is there now, so a connection can offer it */
}

static void shutdown_plugin(kr_daemon *d)
{
    if (cb.source)
        kr_daemon_remove_source(d, &cb);
    if (cb.x)
        kr_x11_remove_handler(cb.x, d);
    free(cb.content);
    free(cb.own);
    memset(&cb, 0, sizeof cb);
}

/* ---- the plugin ----------------------------------------------------------------------- */

static void on_connect(kr_daemon *d, kr_device *dev)
{
    const kr_config *cfg = kr_daemon_config(d);
    if (!cb.x || !cb.content || !cb.content[0] || !kr_config_get_bool(cfg, "clipboard", "auto_share", true) ||
        (cb.secret_content && !kr_config_get_bool(cfg, "clipboard", "share_passwords", false)))
        return;
    kr_packet *p = kr_packet_new(CONNECT);
    if (!p)
        return;
    cJSON_AddStringToObject(p->body, "content", cb.content);
    cJSON_AddNumberToObject(p->body, "timestamp", (double)cb.stamp_ms);
    kr_daemon_send(d, dev, p);
    kr_packet_free(p);
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)dev;
    const char *content = kr_packet_str(p, "content");
    if (!cb.x || !content || !kr_utf8_valid(content, strlen(content)))
        return;
    if (strcmp(p->type, CONNECT) == 0) {
        int64_t ts = 0;
        /* 0 means the sender doesn't know; older than ours means ours is newer. */
        if (!kr_packet_int(p, "timestamp", &ts) || ts == 0 || ts < cb.stamp_ms)
            return;
    }
    if (content[0])
        kr_clipboard_set_text(d, content);
}

const kr_plugin kr_plugin_clipboard = {
    .name = "clipboard",
    .incoming = types,
    .outgoing = types,
    .init = init,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_packet = on_packet,
};
