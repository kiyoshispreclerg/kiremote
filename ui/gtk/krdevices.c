/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The list of devices: what the window's first page and the KiDesktop page
 * show. Everything it says and offers comes from the kr_model. */
#include <string.h>

#include "i18n.h"
#include "krui_private.h"

typedef struct {
    kr_ui *ui;
    gboolean compact;
    GtkWidget *root, *rows, *notice;
    char expanded[64]; /* the device whose actions are open, or "" */
    guint listener, rebuild_idle;
} devs;

/* The host's colors, if it has any (krui.h). */
static void tint_fg(devs *dv, GtkWidget *w)
{
    GdkColor fg, bg;
    if (kr_ui_colors(dv->ui, &fg, &bg))
        gtk_widget_modify_fg(w, GTK_STATE_NORMAL, &fg);
}

static void tint_bg(devs *dv, GtkWidget *w)
{
    GdkColor fg, bg;
    if (kr_ui_colors(dv->ui, &fg, &bg))
        gtk_widget_modify_bg(w, GTK_STATE_NORMAL, &bg);
}

/* The row under the pointer: the host's text color faintly over its
 * background, or the theme's prelight. */
static void hover_color(devs *dv, GtkWidget *w, GdkColor *out)
{
    GdkColor fg, bg;
    if (kr_ui_colors(dv->ui, &fg, &bg)) {
        out->red = (guint16)(bg.red + (fg.red - bg.red) * 0.14);
        out->green = (guint16)(bg.green + (fg.green - bg.green) * 0.14);
        out->blue = (guint16)(bg.blue + (fg.blue - bg.blue) * 0.14);
    } else {
        *out = gtk_widget_get_style(w)->bg[GTK_STATE_PRELIGHT];
    }
}

static gboolean row_crossing(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    devs *dv = data;
    /* Moving onto a child (a button) is still inside the row. */
    if (ev->detail == GDK_NOTIFY_INFERIOR)
        return FALSE;
    if (ev->type == GDK_ENTER_NOTIFY) {
        GdkColor c;
        hover_color(dv, w, &c);
        gtk_widget_modify_bg(w, GTK_STATE_NORMAL, &c);
    } else {
        GdkColor fg, bg;
        if (kr_ui_colors(dv->ui, &fg, &bg))
            gtk_widget_modify_bg(w, GTK_STATE_NORMAL, &bg);
        else
            gtk_widget_modify_bg(w, GTK_STATE_NORMAL, NULL);
    }
    return FALSE;
}

static GtkWindow *toplevel_of(GtkWidget *w)
{
    GtkWidget *t = gtk_widget_get_toplevel(w);
    return GTK_WIDGET_TOPLEVEL(t) ? GTK_WINDOW(t) : NULL;
}

static void schedule_rebuild(devs *dv);

/* ---- one row ---------------------------------------------------------------- */

typedef struct {
    devs *d;
    char id[64];
    kr_action action;
} click_ctx;

static void click_free(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void inline_clicked(GtkButton *b, gpointer data)
{
    click_ctx *c = data;
    kr_ui_run_action(c->d->ui, toplevel_of(GTK_WIDGET(b)), c->action, c->id);
}

/* Clicking the row opens (or closes) its actions under the data. */
static gboolean row_pressed(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    click_ctx *c = data;
    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (!strcmp(c->d->expanded, c->id))
        c->d->expanded[0] = 0;
    else
        g_snprintf(c->d->expanded, sizeof c->d->expanded, "%s", c->id);
    schedule_rebuild(c->d);
    return TRUE;
}

static void uris_dropped(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel, guint info,
                         guint time, gpointer data)
{
    click_ctx *c = data;
    (void)x;
    (void)y;
    (void)info;
    char **uris = gtk_selection_data_get_uris(sel);
    char **paths = kr_ui_paths_from_uris(uris);
    if (paths[0])
        kr_ui_send_files(c->d->ui, toplevel_of(w), c->id, (const char *const *)paths);
    g_strfreev(paths);
    g_strfreev(uris);
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

static char *detail_text(const kr_dev *d)
{
    GString *s = g_string_new(kr_dev_state_text(d));
    char buf[64];
    if (d->paired && d->reachable) {
        if (*kr_dev_battery_text(d, buf, sizeof buf))
            g_string_append_printf(s, " \302\267 %s", buf);
        if (*kr_dev_signal_text(d, buf, sizeof buf))
            g_string_append_printf(s, " \302\267 %s", buf);
    }
    if (d->key[0]) {
        char *key = g_strdup_printf(_("Verification key: %s"), d->key);
        g_string_append_printf(s, " \302\267 %s", key);
        g_free(key);
    }
    return g_string_free(s, FALSE);
}

/* A themed icon for each action; the label stands in if the theme has none. */
static const char *action_icon(kr_action a)
{
    switch (a) {
    case KR_ACT_PING: return "dialog-information";
    case KR_ACT_FIND: return "audio-volume-high";
    case KR_ACT_SEND_FILES: return "document-send";
    case KR_ACT_SEND_TEXT: return "mail-send";
    case KR_ACT_BROWSE: return "folder-remote";
    case KR_ACT_UNPAIR: return "edit-delete";
    default: return NULL;
    }
}

/* What you do with the phone, then what undoes the pairing (as in the menu). */
static const kr_action expand_acts[] = {KR_ACT_PING, KR_ACT_FIND, KR_ACT_SEND_FILES, KR_ACT_SEND_TEXT, KR_ACT_BROWSE,
                                        KR_ACT_UNPAIR};
#define PAIRING_ACTS ((1u << KR_ACT_PAIR) | (1u << KR_ACT_ACCEPT) | (1u << KR_ACT_REFUSE) | (1u << KR_ACT_CANCEL))

static GtkWidget *actions_bar(devs *dv, const kr_dev *d, unsigned acts)
{
    GtkWidget *bar = gtk_hbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar), dv->compact ? 4 : 8);
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    for (size_t i = 0; i < G_N_ELEMENTS(expand_acts); i++) {
        kr_action a = expand_acts[i];
        if (!(acts & (1u << a)))
            continue;
        const char *icon = action_icon(a);
        GtkWidget *b;
        if (icon && gtk_icon_theme_has_icon(theme, icon)) {
            b = gtk_button_new();
            gtk_container_add(GTK_CONTAINER(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
        } else {
            b = gtk_button_new_with_label(kr_action_label(a));
        }
        gtk_widget_set_tooltip_text(b, kr_action_label(a));
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        click_ctx *c = g_new0(click_ctx, 1);
        c->d = dv;
        c->action = a;
        g_snprintf(c->id, sizeof c->id, "%s", d->id);
        g_signal_connect_data(b, "clicked", G_CALLBACK(inline_clicked), c, click_free, 0);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    return bar;
}

static GtkWidget *row_new(devs *dv, const kr_dev *d)
{
    GtkWidget *event = gtk_event_box_new();
    tint_bg(dv, event);
    gtk_widget_add_events(event, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(event, "enter-notify-event", G_CALLBACK(row_crossing), dv);
    g_signal_connect(event, "leave-notify-event", G_CALLBACK(row_crossing), dv);
    unsigned acts = kr_dev_actions(d);
    gboolean can_expand = (acts & ~PAIRING_ACTS) != 0;
    gboolean open = can_expand && !strcmp(dv->expanded, d->id);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), open ? GTK_SHADOW_ETCHED_IN : GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(frame), event);
    GtkWidget *content = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(event), content);
    GtkWidget *hbox = gtk_hbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), dv->compact ? 4 : 8);
    gtk_box_pack_start(GTK_BOX(content), hbox, FALSE, FALSE, 0);

    GtkWidget *icon = gtk_image_new_from_icon_name(kr_dev_icon(d), dv->compact ? GTK_ICON_SIZE_LARGE_TOOLBAR : GTK_ICON_SIZE_DND);
    gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);

    GtkWidget *text = gtk_vbox_new(FALSE, 2);
    char *name = g_markup_printf_escaped("<b>%s</b>", d->name[0] ? d->name : d->id);
    GtkWidget *l1 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l1), name);
    g_free(name);
    gtk_misc_set_alignment(GTK_MISC(l1), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(l1), PANGO_ELLIPSIZE_END);
    tint_fg(dv, l1);
    char *detail = detail_text(d);
    char *dm = g_markup_printf_escaped("<small>%s</small>", detail);
    GtkWidget *l2 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l2), dm);
    g_free(dm);
    g_free(detail);
    gtk_misc_set_alignment(GTK_MISC(l2), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(l2), PANGO_ELLIPSIZE_END);
    tint_fg(dv, l2);
    gtk_box_pack_start(GTK_BOX(text), l1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), l2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), text, TRUE, TRUE, 0);

    /* The pairing answers are right on the row: the question is waiting. */
    static const kr_action inline_acts[] = {KR_ACT_PAIR, KR_ACT_ACCEPT, KR_ACT_REFUSE, KR_ACT_CANCEL};
    for (size_t i = 0; i < G_N_ELEMENTS(inline_acts); i++) {
        if (!(acts & (1u << inline_acts[i])))
            continue;
        GtkWidget *b = gtk_button_new_with_label(kr_action_label(inline_acts[i]));
        click_ctx *c = g_new0(click_ctx, 1);
        c->d = dv;
        c->action = inline_acts[i];
        g_snprintf(c->id, sizeof c->id, "%s", d->id);
        g_signal_connect_data(b, "clicked", G_CALLBACK(inline_clicked), c, click_free, 0);
        gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    }
    /* Everything else, under the data when the row is clicked. */
    if (can_expand) {
        click_ctx *c = g_new0(click_ctx, 1);
        c->d = dv;
        g_snprintf(c->id, sizeof c->id, "%s", d->id);
        g_signal_connect_data(event, "button-press-event", G_CALLBACK(row_pressed), c, click_free, 0);
    }
    if (open)
        gtk_box_pack_start(GTK_BOX(content), actions_bar(dv, d, acts), FALSE, FALSE, 0);

    if (acts & (1u << KR_ACT_SEND_FILES)) {
        static const GtkTargetEntry targets[] = {{"text/uri-list", 0, 0}};
        gtk_drag_dest_set(event, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
        click_ctx *c = g_new0(click_ctx, 1);
        c->d = dv;
        g_snprintf(c->id, sizeof c->id, "%s", d->id);
        g_signal_connect_data(event, "drag-data-received", G_CALLBACK(uris_dropped), c, click_free, 0);
    }
    return frame;
}

/* ---- the list ------------------------------------------------------------------ */

static void rebuild(devs *dv)
{
    GList *old = gtk_container_get_children(GTK_CONTAINER(dv->rows));
    for (GList *l = old; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(old);

    const kr_model *m = kr_ui_model(dv->ui);
    int n = kr_model_count(m);
    if (!kr_model_daemon_up(m)) {
        gtk_label_set_text(GTK_LABEL(dv->notice), _("Kiremote is not running.\nStart kiremoted and this list fills in by itself."));
        gtk_widget_show(dv->notice);
    } else if (!n) {
        gtk_label_set_text(GTK_LABEL(dv->notice),
                           _("No devices around.\nOpen KDE Connect on the phone, on the same network as this computer."));
        gtk_widget_show(dv->notice);
    } else {
        gtk_widget_hide(dv->notice);
    }
    if (dv->expanded[0] && !kr_model_find(m, dv->expanded))
        dv->expanded[0] = 0;
    for (int i = 0; i < n; i++) {
        GtkWidget *row = row_new(dv, kr_model_at(m, i));
        gtk_box_pack_start(GTK_BOX(dv->rows), row, FALSE, FALSE, 0);
        if (i + 1 < n)
            gtk_box_pack_start(GTK_BOX(dv->rows), gtk_hseparator_new(), FALSE, FALSE, 0);
    }
    gtk_widget_show_all(dv->rows);
}

static gboolean rebuild_idle(gpointer data)
{
    devs *dv = data;
    dv->rebuild_idle = 0;
    rebuild(dv);
    return G_SOURCE_REMOVE;
}

static void schedule_rebuild(devs *dv)
{
    if (!dv->rebuild_idle)
        dv->rebuild_idle = g_idle_add(rebuild_idle, dv);
}

static void on_change(void *ud, kr_change what, const char *id)
{
    devs *dv = ud;
    (void)id;
    if (what == KR_CH_DAEMON || what == KR_CH_DEVICES || what == KR_CH_PAIR_REQUEST)
        schedule_rebuild(dv);
}

static void refresh_clicked(GtkButton *b, gpointer data)
{
    devs *dv = data;
    kr_ui_run_action(dv->ui, toplevel_of(GTK_WIDGET(b)), KR_ACT_REFRESH, "");
}

static void root_destroyed(GtkWidget *w, gpointer data)
{
    devs *dv = data;
    (void)w;
    kr_ui_unlisten(dv->ui, dv->listener);
    if (dv->rebuild_idle)
        g_source_remove(dv->rebuild_idle);
    g_free(dv);
}

GtkWidget *kr_ui_devices_new(kr_ui *ui, gboolean compact)
{
    devs *dv = g_new0(devs, 1);
    dv->ui = ui;
    dv->compact = compact;
    dv->root = gtk_vbox_new(FALSE, 6);

    dv->notice = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(dv->notice), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(dv->notice), TRUE);
    gtk_misc_set_padding(GTK_MISC(dv->notice), 12, 16);
    tint_fg(dv, dv->notice);
    gtk_widget_set_no_show_all(dv->notice, TRUE);
    gtk_box_pack_start(GTK_BOX(dv->root), dv->notice, FALSE, FALSE, 0);

    dv->rows = gtk_vbox_new(FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(sw), dv->rows);
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(GTK_BIN(sw)->child), GTK_SHADOW_NONE);
    tint_bg(dv, GTK_BIN(sw)->child);
    tint_bg(dv, dv->rows);
    gtk_box_pack_start(GTK_BOX(dv->root), sw, TRUE, TRUE, 0);

    GtkWidget *bar = gtk_hbox_new(FALSE, 6);
    GtkWidget *refresh = gtk_button_new_with_label(kr_action_label(KR_ACT_REFRESH));
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), dv);
    gtk_box_pack_end(GTK_BOX(bar), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dv->root), bar, FALSE, FALSE, compact ? 2 : 6);
    g_object_set_data(G_OBJECT(dv->root), "kr-footer", bar);

    dv->listener = kr_ui_listen(ui, on_change, dv);
    g_signal_connect(dv->root, "destroy", G_CALLBACK(root_destroyed), dv);
    rebuild(dv);
    gtk_widget_show_all(dv->root);
    return dv->root;
}

void kr_ui_devices_add_footer(GtkWidget *devices, GtkWidget *button)
{
    GtkWidget *bar = g_object_get_data(G_OBJECT(devices), "kr-footer");
    if (bar)
        gtk_box_pack_start(GTK_BOX(bar), button, FALSE, FALSE, 0);
}
