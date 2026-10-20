/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The xispanel widget (`WIDGET <panel> <order> kiremote`): the phone icon on
 * the bar, with how many devices are connected (or a "!" when one asks to
 * pair) and the batteries as tooltip. A click opens the xisserve page
 * (`xisserve --kiremote`).
 *
 * It runs inside xispanel: no GTK, no sockets, nothing that blocks. What it
 * knows comes from the status file the daemon keeps (kiremote-status.json),
 * read every couple of seconds with the same krmodel the other interfaces
 * use. A pairing request is asked with `xisserve --question`, once. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "krmodel.h"
#include "util.h"
#include "xispanel_widget.h"

#define TICK_MS 2000
#define MAX_ASKED 8
#define MARKER_PREFIX "kiremote-asked-"
/* Markers a panel that died left behind. */
#define MARKER_MAX_AGE_S 3600

typedef struct {
    kr_model *model;
    char status_path[KR_PATH_MAX];
    char icon_name[64];
    cairo_surface_t *icon;
    int icon_px;
    struct {
        char id[64];
        char marker[KR_PATH_MAX];
    } asked[MAX_ASKED];
    int n_asked;
} priv;

/* ---- asking a pairing question ------------------------------------------------------ */

/* in, quoted for `sh -c` as one word. */
static void sh_quote(const char *in, char *out, size_t size)
{
    size_t n = 0;
    if (size < 3) {
        if (size)
            out[0] = '\0';
        return;
    }
    out[n++] = '\'';
    for (; *in && n + 5 < size; in++) {
        if (*in == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = *in;
        }
    }
    out[n++] = '\'';
    out[n] = '\0';
}

/* Ids and keys come from a phone: only what the protocol allows ends up in a
 * file name. */
static int safe_word(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_' || *s == '-'))
            return 0;
    return 1;
}

/* Only the first panel to ask (a bar on each monitor shows this widget too,
 * and xispanel may restart) gets to: whoever creates the marker. */
static int claim(const char *marker)
{
    int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static void ask(const kr_dev *d, const char *marker, priv *p)
{
    char text[600], t[1300], id[160], cmd[2600];
    snprintf(text, sizeof text, _("%s wants to pair"), d->name[0] ? d->name : d->id);
    size_t n = strlen(text);
    /* The same two messages as the pairing dialog of the window. */
    snprintf(text + n, sizeof text - n, "\n\n");
    n += 2;
    snprintf(text + n, sizeof text - n,
             _("Verification key: %s\n\nCheck that the other device shows the same key before accepting."), d->key);
    char refuse[160], accept[160], r[300], a[300];
    snprintf(refuse, sizeof refuse, "%s:0", kr_action_label(KR_ACT_REFUSE));
    snprintf(accept, sizeof accept, "%s:1", kr_action_label(KR_ACT_ACCEPT));
    sh_quote(text, t, sizeof t);
    sh_quote(d->id, id, sizeof id);
    sh_quote(refuse, r, sizeof r);
    sh_quote(accept, a, sizeof a);
    /* Dismissing the popup (no answer) leaves the request as it is. */
    snprintf(cmd, sizeof cmd,
             "{ if r=$(xisserve --question --text=%s --button=%s --button=%s); then "
             "if [ \"$r\" = 1 ]; then kiremote-cli -d %s --pair; else kiremote-cli -d %s --unpair; fi; fi; } "
             ">/dev/null 2>&1",
             t, r, a, id, id);
    xpw_run(cmd);
    if (p->n_asked < MAX_ASKED) {
        snprintf(p->asked[p->n_asked].id, sizeof p->asked[0].id, "%s", d->id);
        snprintf(p->asked[p->n_asked].marker, sizeof p->asked[0].marker, "%s", marker);
        p->n_asked++;
    }
}

/* After a tick: ask for the requests nobody has asked about, and forget the
 * ones that ended. */
static void pairing(priv *p)
{
    const kr_model *m = p->model;
    for (int i = 0; i < p->n_asked;) {
        const kr_dev *d = kr_model_find(m, p->asked[i].id);
        if (d && strcmp(d->state, "requested_by_peer") == 0) {
            i++;
            continue;
        }
        unlink(p->asked[i].marker);
        p->asked[i] = p->asked[--p->n_asked];
    }
    for (int i = 0; i < kr_model_count(m); i++) {
        const kr_dev *d = kr_model_at(m, i);
        if (strcmp(d->state, "requested_by_peer") != 0 || !safe_word(d->id) || !safe_word(d->key))
            continue;
        char name[160], marker[KR_PATH_MAX];
        snprintf(name, sizeof name, MARKER_PREFIX "%s-%s", d->id, d->key);
        if (!kr_runtime_path(marker, sizeof marker, name))
            continue;
        int mine = 0;
        for (int k = 0; k < p->n_asked; k++)
            mine |= strcmp(p->asked[k].id, d->id) == 0;
        if (!mine && claim(marker))
            ask(d, marker, p); /* if claim failed, another panel asked */
    }
}

static void sweep_old_markers(void)
{
    char dir[KR_PATH_MAX];
    if (!getenv("XDG_RUNTIME_DIR") || !kr_pathf(dir, sizeof dir, "%s", getenv("XDG_RUNTIME_DIR")))
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    time_t now = time(NULL);
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[KR_PATH_MAX];
        struct stat st;
        if (strncmp(e->d_name, MARKER_PREFIX, strlen(MARKER_PREFIX)) == 0 &&
            kr_pathf(path, sizeof path, "%s/%s", dir, e->d_name) && stat(path, &st) == 0 &&
            now - st.st_mtime > MARKER_MAX_AGE_S)
            unlink(path);
    }
    closedir(d);
}

/* ---- the widget ------------------------------------------------------------------------ */

static int init(XpwWidget *w)
{
    priv *p = xpw_priv(w);
    kr_i18n_bind();
    p->model = kr_model_new();
    if (!p->model)
        return -1;
    if (!xpw_option(w, "icon", p->icon_name, sizeof p->icon_name) || !p->icon_name[0])
        snprintf(p->icon_name, sizeof p->icon_name, "phone");
    if (!kr_model_status_path(p->status_path, sizeof p->status_path))
        p->status_path[0] = '\0';
    sweep_old_markers();
    xpw_tick_at(w, xpw_now_ms() + 100);
    return 0;
}

static void destroy(XpwWidget *w)
{
    priv *p = xpw_priv(w);
    if (p->icon)
        cairo_surface_destroy(p->icon);
    kr_model_free(p->model);
}

static void measure(XpwWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    (void)w;
    *out_len = cross_axis;
    *out_min_len = cross_axis;
}

static void badge(XpwWidget *w, cairo_t *cr, double cx, double cy, double r, const char *text, const double rgb[3])
{
    cairo_save(cr);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    cairo_arc(cr, cx, cy, r, 0, 6.2831853);
    cairo_fill(cr);
    double fg[4], bg[4];
    xpw_colors(w, fg, bg);
    cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
    double tw = xpw_text_width(w, text);
    xpw_draw_text(w, cr, cx - tw / 2, cy - r, 2 * r, text);
    cairo_restore(cr);
}

static void paint(XpwWidget *w, cairo_t *cr)
{
    priv *p = xpw_priv(w);
    int x, y, width, height;
    xpw_geometry(w, &x, &y, &width, &height);
    xpw_paint_hover_bg(w, cr);

    int px = (width < height ? width : height) - 6;
    if (px < 8)
        px = 8;
    if (!p->icon || p->icon_px != px) {
        if (p->icon)
            cairo_surface_destroy(p->icon);
        p->icon = xpw_load_icon(p->icon_name, px);
        p->icon_px = px;
    }
    double ix = x + (width - px) / 2.0, iy = y + (height - px) / 2.0;
    int up = kr_model_daemon_up(p->model);

    /* Faded while there is no daemon to talk to. */
    cairo_push_group(cr);
    if (p->icon)
        xpw_draw_icon(cr, p->icon, ix, iy, px);
    else
        xpw_draw_fallback_icon(w, cr, ix, iy, px, "K");
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, up ? 1.0 : 0.4);

    double r = xpw_text_size(w) * 0.6;
    if (r < 6)
        r = 6;
    if (up && kr_model_pair_pending(p->model)) {
        static const double amber[3] = {0.95, 0.55, 0.1};
        badge(w, cr, ix + px - r * 0.9, iy + r * 0.6, r, "!", amber);
    } else if (up && kr_model_connected(p->model) > 0) {
        double fg[4], bg[4];
        char n[16];
        xpw_colors(w, fg, bg);
        snprintf(n, sizeof n, "%d", kr_model_connected(p->model));
        badge(w, cr, ix + px - r * 0.9, iy + px - r * 0.6, r, n, (double[3]){fg[0], fg[1], fg[2]});
    }
}

static int on_button(XpwWidget *w, int button, int lx, int ly)
{
    (void)lx;
    (void)ly;
    if (button == 1) {
        xpw_open_xisserve(w, NULL, "--kiremote");
        return 1;
    }
    if (button == 2) {
        xpw_run("kiconf --tab kiremote");
        return 1;
    }
    return 0;
}

static int on_tick(XpwWidget *w, uint64_t now_ms)
{
    priv *p = xpw_priv(w);
    int changed = p->status_path[0] && kr_model_load_file(p->model, p->status_path);
    pairing(p);
    xpw_tick_at(w, now_ms + TICK_MS);
    return changed;
}

static int is_urgent(XpwWidget *w)
{
    priv *p = xpw_priv(w);
    return kr_model_daemon_up(p->model) && kr_model_pair_pending(p->model);
}

static int get_tooltip(XpwWidget *w, char *buf, size_t size)
{
    priv *p = xpw_priv(w);
    kr_model_tooltip(p->model, buf, size);
    return 1;
}

static const XispanelWidgetModule mod = {
    .abi = XISPANEL_WIDGET_ABI,
    .priv_size = sizeof(priv),
    .embeddable = 1,
    .init = init,
    .destroy = destroy,
    .measure = measure,
    .paint = paint,
    .on_button = on_button,
    .on_tick = on_tick,
    .is_urgent = is_urgent,
    .get_tooltip = get_tooltip,
};

const XispanelWidgetModule *xispanel_widget_entry(void); /* looked up by xispanel */

const XispanelWidgetModule *xispanel_widget_entry(void)
{
    return &mod;
}
