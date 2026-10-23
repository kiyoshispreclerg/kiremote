/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What a click on an action does: ask for what is missing, confirm what
 * undoes something, send the request, say if it failed. Also the menu of a
 * device's actions. */
#include <string.h>

#include "i18n.h"
#include "krui_private.h"

/* ---- the dialogs a user answers ------------------------------------------- */

static gboolean default_confirm(GtkWindow *parent, const char *question)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s",
                                          question);
    gboolean yes = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(d);
    return yes;
}

static char **default_choose_files(GtkWindow *parent)
{
    GtkWidget *d = gtk_file_chooser_dialog_new(_("Send files"), parent, GTK_FILE_CHOOSER_ACTION_OPEN, GTK_STOCK_CANCEL,
                                               GTK_RESPONSE_CANCEL, _("_Send"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(d), TRUE);
    char **out = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        GSList *l = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d));
        guint n = g_slist_length(l);
        out = g_new0(char *, n + 1);
        guint i = 0;
        for (GSList *it = l; it; it = it->next)
            out[i++] = it->data; /* ours now */
        g_slist_free(l);
    }
    gtk_widget_destroy(d);
    return out;
}

static char *default_ask_text(GtkWindow *parent)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(_("Send text"), parent, GTK_DIALOG_MODAL, GTK_STOCK_CANCEL,
                                               GTK_RESPONSE_CANCEL, _("_Send"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_widget_set_size_request(sw, 360, 120);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), sw, TRUE, TRUE, 6);
    gtk_widget_show_all(sw);
    char *text = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        GtkTextIter a, b;
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
        gtk_text_buffer_get_bounds(buf, &a, &b);
        text = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    }
    gtk_widget_destroy(d);
    return text;
}

static void default_show_error(GtkWindow *parent, const char *message)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static gboolean default_spawn(GtkWindow *parent, const char *const *argv)
{
    GError *err = NULL;
    if (g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &err))
        return TRUE;
    char *msg = g_strdup_printf(_("Could not run %s: %s"), argv[0], err->message);
    default_show_error(parent, msg);
    g_free(msg);
    g_error_free(err);
    return FALSE;
}

void kr_ui_default_hooks(kr_ui_hooks *h)
{
    *h = (kr_ui_hooks){default_confirm, default_choose_files, default_ask_text, default_show_error, default_spawn};
}

/* ---- sending ------------------------------------------------------------------ */

typedef struct {
    kr_ui *ui;
    GtkWindow *parent; /* NULL once it is gone */
} req_ctx;

static void on_reply(void *ud, const cJSON *reply)
{
    req_ctx *c = ud;
    const char *err = kr_reply_error(reply);
    if (err && !kr_ui_closing(c->ui))
        kr_ui_get_hooks(c->ui)->show_error(c->parent, err);
    if (c->parent)
        g_object_remove_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
    g_free(c);
}

void kr_ui_request(kr_ui *ui, GtkWindow *parent, cJSON *req)
{
    req_ctx *c = g_new0(req_ctx, 1);
    c->ui = ui;
    c->parent = parent;
    if (parent)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&c->parent);
    kr_client_send(kr_ui_client(ui), req, on_reply, c);
}

void kr_ui_send_files(kr_ui *ui, GtkWindow *parent, const char *device_id, const char *const *paths)
{
    int n = 0;
    while (paths && paths[n])
        n++;
    cJSON *req = kr_action_files(device_id, paths, n);
    if (req)
        kr_ui_request(ui, parent, req);
}

char **kr_ui_paths_from_uris(char **uris)
{
    GPtrArray *out = g_ptr_array_new();
    for (; uris && *uris; uris++) {
        char *u = g_strstrip(g_strdup(*uris));
        char *path = *u ? g_filename_from_uri(u, NULL, NULL) : NULL;
        g_free(u);
        if (path)
            g_ptr_array_add(out, path);
    }
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

void kr_ui_run_action(kr_ui *ui, GtkWindow *parent, kr_action action, const char *device_id)
{
    const kr_ui_hooks *h = kr_ui_get_hooks(ui);
    const kr_dev *d = kr_model_find(kr_ui_model(ui), device_id);
    char *name = g_strdup(d && d->name[0] ? d->name : device_id);
    char *id = g_strdup(device_id); /* the hooks may run a nested loop that updates the model */

    if (action == KR_ACT_MIRROR) {
        /* Its own process and window; it finds the phone through adb. */
        const char *prog = kr_ui_mirror_program();
        if (prog) {
            const char *argv[] = {prog, NULL};
            h->spawn(parent, argv);
        }
    } else if (action == KR_ACT_SEND_FILES) {
        char **paths = h->choose_files(parent);
        if (paths && paths[0])
            kr_ui_send_files(ui, parent, id, (const char *const *)paths);
        g_strfreev(paths);
    } else if (action == KR_ACT_SEND_TEXT) {
        char *text = h->ask_text(parent);
        cJSON *req = text ? kr_action_request(action, id, text) : NULL;
        if (req)
            kr_ui_request(ui, parent, req);
        g_free(text);
    } else {
        gboolean go = TRUE;
        if (kr_action_destructive(action)) {
            char *q = g_strdup_printf(action == KR_ACT_REFUSE ? _("Refuse the pairing request from %s?")
                                                              : _("Unpair from %s?"),
                                      name);
            go = h->confirm(parent, q);
            g_free(q);
        }
        cJSON *req = go ? kr_action_request(action, id, NULL) : NULL;
        if (req)
            kr_ui_request(ui, parent, req);
    }
    g_free(name);
    g_free(id);
}

/* ---- the menu --------------------------------------------------------------------- */

typedef struct {
    kr_ui *ui;
    GtkWindow *parent;
    kr_action action;
    char id[64];
} item_ctx;

static void item_free(gpointer data, GClosure *closure)
{
    item_ctx *c = data;
    (void)closure;
    if (c->parent)
        g_object_remove_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
    g_free(c);
}

static void item_activated(GtkMenuItem *item, gpointer data)
{
    item_ctx *c = data;
    (void)item;
    kr_ui_run_action(c->ui, c->parent, c->action, c->id);
}

static void add_item(GtkWidget *menu, kr_ui *ui, GtkWindow *parent, kr_action a, const char *id)
{
    GtkWidget *mi = gtk_menu_item_new_with_label(kr_action_label(a));
    item_ctx *c = g_new0(item_ctx, 1);
    c->ui = ui;
    c->parent = parent;
    c->action = a;
    g_snprintf(c->id, sizeof c->id, "%s", id);
    if (parent)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&c->parent);
    g_signal_connect_data(mi, "activate", G_CALLBACK(item_activated), c, item_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show(mi);
}

GtkWidget *kr_ui_device_menu_new(kr_ui *ui, GtkWindow *parent, const char *device_id)
{
    GtkWidget *menu = gtk_menu_new();
    const kr_dev *d = kr_model_find(kr_ui_model(ui), device_id);
    unsigned acts = d ? kr_dev_actions(d) : 0;
    /* What you do with the phone, then what concerns the pairing. */
    static const kr_action about_phone[] = {KR_ACT_PING,      KR_ACT_FIND,   KR_ACT_SEND_FILES,
                                            KR_ACT_SEND_TEXT, KR_ACT_BROWSE, KR_ACT_MIRROR};
    static const kr_action pairing[] = {KR_ACT_PAIR, KR_ACT_ACCEPT, KR_ACT_REFUSE, KR_ACT_CANCEL, KR_ACT_UNPAIR};
    int n = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(about_phone); i++)
        if (acts & (1u << about_phone[i])) {
            add_item(menu, ui, parent, about_phone[i], device_id);
            n++;
        }
    gboolean sep = n > 0;
    for (size_t i = 0; i < G_N_ELEMENTS(pairing); i++)
        if (acts & (1u << pairing[i])) {
            if (sep) {
                GtkWidget *s = gtk_separator_menu_item_new();
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), s);
                gtk_widget_show(s);
                sep = FALSE;
            }
            add_item(menu, ui, parent, pairing[i], device_id);
            n++;
        }
    if (!n) {
        GtkWidget *mi = gtk_menu_item_new_with_label(_("Nothing to do"));
        gtk_widget_set_sensitive(mi, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        gtk_widget_show(mi);
    }
    return menu;
}
