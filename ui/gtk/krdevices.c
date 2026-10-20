/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The list of devices: what the window's first page and the KiDesktop page
 * show. Everything it says and offers comes from the kr_model. */
#include <string.h>

#include "i18n.h"
#include "krui_private.h"

typedef struct {
    kr_ui *ui;
    gboolean compact;
    GtkWidget *root, *rows, *notice, *menu;
    guint listener, rebuild_idle;
} devs;

static GtkWindow *toplevel_of(GtkWidget *w)
{
    GtkWidget *t = gtk_widget_get_toplevel(w);
    return GTK_WIDGET_TOPLEVEL(t) ? GTK_WINDOW(t) : NULL;
}

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

static void menu_position(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
    GtkWidget *button = data;
    GtkRequisition req;
    gdk_window_get_origin(button->window, x, y);
    gtk_widget_size_request(GTK_WIDGET(menu), &req);
    *x += button->allocation.x + button->allocation.width - req.width;
    *y += button->allocation.y + button->allocation.height;
    *push_in = TRUE;
}

static void menu_clicked(GtkButton *b, gpointer data)
{
    click_ctx *c = data;
    if (c->d->menu)
        gtk_widget_destroy(c->d->menu);
    c->d->menu = g_object_ref_sink(kr_ui_device_menu_new(c->d->ui, toplevel_of(GTK_WIDGET(b)), c->id));
    gtk_menu_popup(GTK_MENU(c->d->menu), NULL, NULL, menu_position, b, 1, gtk_get_current_event_time());
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

static GtkWidget *row_new(devs *dv, const kr_dev *d)
{
    GtkWidget *event = gtk_event_box_new();
    GtkWidget *hbox = gtk_hbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), dv->compact ? 4 : 8);
    gtk_container_add(GTK_CONTAINER(event), hbox);

    GtkWidget *icon = gtk_image_new_from_icon_name(kr_dev_icon(d), dv->compact ? GTK_ICON_SIZE_LARGE_TOOLBAR : GTK_ICON_SIZE_DND);
    gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);

    GtkWidget *text = gtk_vbox_new(FALSE, 2);
    char *name = g_markup_printf_escaped("<b>%s</b>", d->name[0] ? d->name : d->id);
    GtkWidget *l1 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l1), name);
    g_free(name);
    gtk_misc_set_alignment(GTK_MISC(l1), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(l1), PANGO_ELLIPSIZE_END);
    char *detail = detail_text(d);
    char *dm = g_markup_printf_escaped("<small>%s</small>", detail);
    GtkWidget *l2 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l2), dm);
    g_free(dm);
    g_free(detail);
    gtk_misc_set_alignment(GTK_MISC(l2), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(l2), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(text), l1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), l2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), text, TRUE, TRUE, 0);

    unsigned acts = kr_dev_actions(d);
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
    /* Everything else, in the menu (the pairing answers are not repeated). */
    unsigned menu_acts = acts & ~((1u << KR_ACT_PAIR) | (1u << KR_ACT_ACCEPT) | (1u << KR_ACT_REFUSE) | (1u << KR_ACT_CANCEL));
    if (menu_acts) {
        GtkWidget *b = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(b), gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_NONE));
        gtk_widget_set_tooltip_text(b, _("More actions"));
        click_ctx *c = g_new0(click_ctx, 1);
        c->d = dv;
        g_snprintf(c->id, sizeof c->id, "%s", d->id);
        g_signal_connect_data(b, "clicked", G_CALLBACK(menu_clicked), c, click_free, 0);
        gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    }

    if (acts & (1u << KR_ACT_SEND_FILES)) {
        static const GtkTargetEntry targets[] = {{"text/uri-list", 0, 0}};
        gtk_drag_dest_set(event, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
        click_ctx *c = g_new0(click_ctx, 1);
        c->d = dv;
        g_snprintf(c->id, sizeof c->id, "%s", d->id);
        g_signal_connect_data(event, "drag-data-received", G_CALLBACK(uris_dropped), c, click_free, 0);
    }
    return event;
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

static void on_change(void *ud, kr_change what, const char *id)
{
    devs *dv = ud;
    (void)id;
    if ((what == KR_CH_DAEMON || what == KR_CH_DEVICES || what == KR_CH_PAIR_REQUEST) && !dv->rebuild_idle)
        dv->rebuild_idle = g_idle_add(rebuild_idle, dv);
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
    if (dv->menu)
        gtk_widget_destroy(dv->menu);
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
    gtk_widget_set_no_show_all(dv->notice, TRUE);
    gtk_box_pack_start(GTK_BOX(dv->root), dv->notice, FALSE, FALSE, 0);

    dv->rows = gtk_vbox_new(FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(sw), dv->rows);
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(GTK_BIN(sw)->child), GTK_SHADOW_NONE);
    gtk_box_pack_start(GTK_BOX(dv->root), sw, TRUE, TRUE, 0);

    GtkWidget *bar = gtk_hbox_new(FALSE, 6);
    GtkWidget *refresh = gtk_button_new_with_label(kr_action_label(KR_ACT_REFRESH));
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), dv);
    gtk_box_pack_end(GTK_BOX(bar), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dv->root), bar, FALSE, FALSE, compact ? 2 : 6);

    dv->listener = kr_ui_listen(ui, on_change, dv);
    g_signal_connect(dv->root, "destroy", G_CALLBACK(root_destroyed), dv);
    rebuild(dv);
    gtk_widget_show_all(dv->root);
    return dv->root;
}
