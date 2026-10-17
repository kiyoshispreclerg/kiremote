/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The phone as mouse and keyboard (plugins/mousepad upstream, its X11
 * implementation x11remoteinput.cpp, and and/plugins/mousepad for the
 * sender).
 *
 *   in  kdeconnect.mousepad.request  one of
 *         {"dx","dy"}                          move the pointer by that much
 *         {"x","y"}                            move it to that position
 *         {"singleclick"|"doubleclick"|"middleclick"|"rightclick": true}
 *         {"singlehold": true} {"singlerelease": true}   drag and drop
 *         {"scroll": true, "dy"}               one wheel click, up for dy > 0
 *         {"key": "text"}                      type the text
 *         {"specialKey": n}                    1..32: BackSpace, Tab, ...
 *       the key forms can add "ctrl", "alt", "shift", "super": true
 *   out kdeconnect.mousepad.keyboardstate  {"state": true}  on connection
 *
 * Everything goes through XTest, so programs see ordinary input. The left
 * and right buttons swap if the pointer mapping is left-handed. Characters
 * the keyboard layout lacks (accents, emoji) are typed by lending an unused
 * keycode the right keysym for a moment, which clients need a few
 * milliseconds to notice; so typing goes through a small queue that waits
 * between such characters. Fractions of a pixel in dx and dy are carried
 * over to the next move. Wayland has no XTest; there the plugin is idle. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include "log.h"
#include "plugin.h"
#include "util.h"
#include "x11.h"

#define REQUEST "kdeconnect.mousepad.request"
#define KEYBOARDSTATE "kdeconnect.mousepad.keyboardstate"
#define REMAP_SETTLE_S 0.04
#define QUEUE_MAX 4096

static const char *const incoming[] = {REQUEST, NULL};
static const char *const outgoing[] = {KEYBOARDSTATE, NULL};

/* SpecialKeysMap of x11remoteinput.cpp: keep in sync with the Android
 * KeyListenerView.SpecialKeysMap. */
static const KeySym special_keys[] = {
    0,           XK_BackSpace, XK_Tab,       XK_Linefeed,  XK_Left,      XK_Up,       XK_Right,
    XK_Down,     XK_Page_Up,   XK_Page_Down, XK_Home,      XK_End,       XK_Return,   XK_Delete,
    XK_Escape,   XK_Sys_Req,   XK_Scroll_Lock, XK_Control_L, XK_Alt_L,   XK_Shift_L,  XK_Super_L,
    XK_F1,       XK_F2,        XK_F3,        XK_F4,        XK_F5,        XK_F6,       XK_F7,
    XK_F8,       XK_F9,        XK_F10,       XK_F11,       XK_F12,
};

enum { LEFT = 1, MIDDLE = 2, RIGHT = 3, WHEEL_UP = 4, WHEEL_DOWN = 5 };

typedef struct {
    enum { OP_PRESS_KC, OP_RELEASE_KC, OP_TYPE_SYM } op;
    unsigned long v; /* keycode or keysym */
} item;

static struct {
    kr_x11 *x;
    Display *dpy;
    double rem_x, rem_y; /* fractions of a pixel not moved yet */
    item *q;
    size_t q_head, q_len, q_cap;
    KeyCode lent;         /* the keycode currently lent, 0 if none */
    double restore_at;    /* when to give it back, 0 if none */
    bool source;
} mp;

/* ---- the key queue ------------------------------------------------------------- */

static void fake_key(KeyCode kc, bool down)
{
    XTestFakeKeyEvent(mp.dpy, kc, down, 0);
}

/* An unused keycode, or 0. */
static KeyCode spare_keycode(void)
{
    int min, max, per;
    XDisplayKeycodes(mp.dpy, &min, &max);
    KeySym *map = XGetKeyboardMapping(mp.dpy, (KeyCode)min, max - min + 1, &per);
    KeyCode found = 0;
    for (int kc = max; map && kc >= min && !found; kc--) {
        bool empty = true;
        for (int i = 0; i < per && empty; i++)
            empty = map[(kc - min) * per + i] == NoSymbol;
        if (empty)
            found = (KeyCode)kc;
    }
    if (map)
        XFree(map);
    return found;
}

/* Types one keysym. Returns true if it had to lend a keycode (so the queue
 * must wait before the next one). */
static bool type_sym(KeySym ks)
{
    KeyCode kc = XKeysymToKeycode(mp.dpy, ks);
    if (kc) {
        int level = -1;
        for (int l = 0; l < 2 && level < 0; l++)
            if (XkbKeycodeToKeysym(mp.dpy, kc, 0, l) == ks)
                level = l;
        if (level >= 0) {
            KeyCode shift = XKeysymToKeycode(mp.dpy, XK_Shift_L);
            if (level == 1 && shift)
                fake_key(shift, true);
            fake_key(kc, true);
            fake_key(kc, false);
            if (level == 1 && shift)
                fake_key(shift, false);
            return false;
        }
    }
    KeyCode spare = spare_keycode();
    if (!spare) {
        kr_log_debug("mousepad: no free keycode to type keysym 0x%lx", (unsigned long)ks);
        return false;
    }
    XChangeKeyboardMapping(mp.dpy, spare, 1, &ks, 1);
    XSync(mp.dpy, False);
    fake_key(spare, true);
    fake_key(spare, false);
    mp.lent = spare;
    mp.restore_at = kr_now() + REMAP_SETTLE_S;
    return true;
}

static void restore_lent(void)
{
    if (!mp.lent)
        return;
    KeySym none = NoSymbol;
    XChangeKeyboardMapping(mp.dpy, mp.lent, 1, &none, 1);
    mp.lent = 0;
    mp.restore_at = 0;
}

/* Runs the queue until it is empty or has to wait. */
static void run_queue(void)
{
    while (mp.q_head < mp.q_len && !mp.restore_at) {
        item it = mp.q[mp.q_head++];
        switch (it.op) {
        case OP_PRESS_KC:
            fake_key((KeyCode)it.v, true);
            break;
        case OP_RELEASE_KC:
            fake_key((KeyCode)it.v, false);
            break;
        case OP_TYPE_SYM:
            type_sym((KeySym)it.v);
            break;
        }
    }
    if (mp.q_head == mp.q_len)
        mp.q_head = mp.q_len = 0;
    XFlush(mp.dpy);
}

static bool enqueue(int op, unsigned long v)
{
    if (mp.q_len - mp.q_head >= QUEUE_MAX)
        return false;
    if (mp.q_len == mp.q_cap) {
        size_t cap = mp.q_cap ? mp.q_cap * 2 : 64;
        item *n = realloc(mp.q, cap * sizeof *n);
        if (!n)
            return false;
        mp.q = n;
        mp.q_cap = cap;
    }
    mp.q[mp.q_len++] = (item){op, v};
    return true;
}

/* The keysym for a Unicode code point, as XStringToKeysym would give. */
static KeySym keysym_for(unsigned cp)
{
    if (cp == '\n')
        return XK_Return;
    if (cp == '\t')
        return XK_Tab;
    if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff))
        return cp;
    return 0x01000000u + cp;
}

/* The next code point of a UTF-8 string; *s moves past it. */
static unsigned next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp;
    int n;
    if (p[0] < 0x80) {
        cp = p[0];
        n = 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        n = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        n = 3;
    } else {
        cp = p[0] & 0x07;
        n = 4;
    }
    for (int i = 1; i < n; i++)
        cp = (cp << 6) | (p[i] & 0x3F);
    *s += n;
    return cp;
}

static double src_deadline(void *ud)
{
    (void)ud;
    return mp.restore_at;
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    (void)ud;
    (void)fds;
    (void)n;
    if (mp.restore_at && kr_now() >= mp.restore_at) {
        restore_lent();
        run_queue();
    }
}

/* ---- the packet ------------------------------------------------------------------ */

static double num(const kr_packet *p, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p->body, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : fallback;
}

static bool left_handed(void)
{
    unsigned char map[20];
    int n = XGetPointerMapping(mp.dpy, map, 20);
    if (n == 1)
        return false;
    if (n == 2)
        return map[0] == 2 && map[1] == 1;
    return map[0] == 3 && map[2] == 1;
}

static void click(int button, int times)
{
    for (int i = 0; i < times; i++) {
        XTestFakeButtonEvent(mp.dpy, (unsigned)button, True, 0);
        XTestFakeButtonEvent(mp.dpy, (unsigned)button, False, 0);
    }
}

static void keys(const kr_packet *p, const char *text, int special)
{
    static const struct {
        const char *field;
        KeySym sym;
    } mods[] = {{"ctrl", XK_Control_L}, {"alt", XK_Alt_L}, {"shift", XK_Shift_L}, {"super", XK_Super_L}};
    KeyCode held[4];
    int n_held = 0;
    for (size_t i = 0; i < 4; i++)
        if (kr_packet_bool(p, mods[i].field, false)) {
            KeyCode kc = XKeysymToKeycode(mp.dpy, mods[i].sym);
            if (kc && enqueue(OP_PRESS_KC, kc))
                held[n_held++] = kc;
        }
    if (special > 0 && special < (int)(sizeof special_keys / sizeof *special_keys)) {
        KeyCode kc = XKeysymToKeycode(mp.dpy, special_keys[special]);
        if (kc) {
            enqueue(OP_PRESS_KC, kc);
            enqueue(OP_RELEASE_KC, kc);
        }
    } else if (text) {
        for (const char *s = text; *s;)
            if (!enqueue(OP_TYPE_SYM, keysym_for(next_cp(&s))))
                break;
    }
    for (int i = n_held - 1; i >= 0; i--)
        enqueue(OP_RELEASE_KC, held[i]);
    run_queue();
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)d;
    (void)dev;
    if (!mp.x)
        return;
    double dx = num(p, "dx", 0), dy = num(p, "dy", 0);
    bool lh = left_handed();
    int main_button = lh ? RIGHT : LEFT, second_button = lh ? LEFT : RIGHT;
    const char *key = kr_packet_str(p, "key");
    int64_t special = 0;
    kr_packet_int(p, "specialKey", &special);
    bool valid_special = special > 0 && special < (int64_t)(sizeof special_keys / sizeof *special_keys);

    if (kr_packet_bool(p, "singleclick", false))
        click(main_button, 1);
    else if (kr_packet_bool(p, "doubleclick", false))
        click(main_button, 2);
    else if (kr_packet_bool(p, "middleclick", false))
        click(MIDDLE, 1);
    else if (kr_packet_bool(p, "rightclick", false))
        click(second_button, 1);
    else if (kr_packet_bool(p, "singlehold", false))
        XTestFakeButtonEvent(mp.dpy, (unsigned)main_button, True, 0); /* drag and drop */
    else if (kr_packet_bool(p, "singlerelease", false))
        XTestFakeButtonEvent(mp.dpy, (unsigned)main_button, False, 0);
    else if (kr_packet_bool(p, "scroll", false)) {
        if (dy < 0)
            click(WHEEL_DOWN, 1);
        else if (dy > 0)
            click(WHEEL_UP, 1);
    } else if ((key && key[0]) || valid_special) {
        keys(p, key && key[0] ? key : NULL, valid_special ? (int)special : 0);
    } else if (dx != 0 || dy != 0) {
        double tx = dx + mp.rem_x, ty = dy + mp.rem_y;
        int mx = (int)tx, my = (int)ty;
        mp.rem_x = tx - mx;
        mp.rem_y = ty - my;
        if (mx || my)
            XTestFakeRelativeMotionEvent(mp.dpy, mx, my, 0);
    } else if (cJSON_GetObjectItemCaseSensitive(p->body, "x") || cJSON_GetObjectItemCaseSensitive(p->body, "y")) {
        XTestFakeMotionEvent(mp.dpy, -1, (int)num(p, "x", 0), (int)num(p, "y", 0), 0);
    }
    XFlush(mp.dpy);
}

/* ---- the plugin --------------------------------------------------------------------- */

static void init(kr_daemon *d)
{
    kr_x11 *x = kr_daemon_x11(d);
    if (!x)
        return;
    int ev, err, maj, min;
    if (!XTestQueryExtension(kr_x11_display(x), &ev, &err, &maj, &min)) {
        kr_log_warn("the X server has no XTest: the phone cannot be a mouse or keyboard");
        return;
    }
    mp.x = x;
    mp.dpy = kr_x11_display(x);
    kr_daemon_add_source(d, &(kr_source){NULL, src_dispatch, src_deadline}, &mp);
    mp.source = true;
}

static void shutdown_plugin(kr_daemon *d)
{
    if (mp.source)
        kr_daemon_remove_source(d, &mp);
    if (mp.x)
        restore_lent();
    free(mp.q);
    memset(&mp, 0, sizeof mp);
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    kr_packet *p = kr_packet_new(KEYBOARDSTATE);
    if (!p)
        return;
    cJSON_AddBoolToObject(p->body, "state", mp.x != NULL);
    kr_daemon_send(d, dev, p);
    kr_packet_free(p);
}

const kr_plugin kr_plugin_mousepad = {
    .name = "mousepad",
    .incoming = incoming,
    .outgoing = outgoing,
    .init = init,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_packet = on_packet,
};
