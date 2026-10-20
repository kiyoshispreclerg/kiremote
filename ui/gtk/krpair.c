/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The pairing question: who asks, and the key to compare with the phone's. */
#include <string.h>

#include "i18n.h"
#include "krui_private.h"

#define ID_KEY "kr-device-id"

typedef struct {
    kr_ui *ui;
    GtkWindow *parent; /* weak */
} watch;

static GtkWidget *find_dialog(kr_ui *ui, const char *id)
{
    for (GList *l = *kr_ui_pair_dialogs(ui); l; l = l->next)
        if (strcmp(g_object_get_data(l->data, ID_KEY), id) == 0)
            return l->data;
    return NULL;
}

static void dialog_response(GtkDialog *dlg, gint response, gpointer data)
{
    kr_ui *ui = data;
    const char *id = g_object_get_data(G_OBJECT(dlg), ID_KEY);
    GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(dlg));
    if (response == GTK_RESPONSE_ACCEPT)
        kr_ui_run_action(ui, parent, KR_ACT_ACCEPT, id);
    else if (response == GTK_RESPONSE_REJECT) /* no "are you sure": the dialog was the question */
        kr_ui_request(ui, parent, kr_action_request(KR_ACT_REFUSE, id, NULL));
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void dialog_destroyed(GtkWidget *w, gpointer data)
{
    kr_ui *ui = data;
    *kr_ui_pair_dialogs(ui) = g_list_remove(*kr_ui_pair_dialogs(ui), w);
}

GtkWidget *kr_ui_pair_dialog(kr_ui *ui, GtkWindow *parent, const char *id)
{
    GtkWidget *dlg = find_dialog(ui, id);
    if (dlg)
        return dlg;
    const kr_dev *d = kr_model_find(kr_ui_model(ui), id);
    if (!d || strcmp(d->state, "requested_by_peer") != 0)
        return NULL;

    dlg = gtk_message_dialog_new(parent, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                 _("%s wants to pair"), d->name[0] ? d->name : d->id);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
                                             _("Verification key: %s\n\nCheck that the other device shows the same key before accepting."),
                                             d->key);
    gtk_dialog_add_button(GTK_DIALOG(dlg), kr_action_label(KR_ACT_REFUSE), GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(dlg), kr_action_label(KR_ACT_ACCEPT), GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    g_object_set_data_full(G_OBJECT(dlg), ID_KEY, g_strdup(id), g_free);
    g_signal_connect(dlg, "response", G_CALLBACK(dialog_response), ui);
    g_signal_connect(dlg, "destroy", G_CALLBACK(dialog_destroyed), ui);
    *kr_ui_pair_dialogs(ui) = g_list_prepend(*kr_ui_pair_dialogs(ui), dlg);
    gtk_window_set_urgency_hint(GTK_WINDOW(dlg), TRUE);
    gtk_widget_show(dlg);
    return dlg;
}

static void show_pending(watch *w)
{
    const kr_model *m = kr_ui_model(w->ui);
    for (int i = 0; i < kr_model_count(m); i++) {
        const kr_dev *d = kr_model_at(m, i);
        if (strcmp(d->state, "requested_by_peer") == 0)
            kr_ui_pair_dialog(w->ui, w->parent, d->id);
    }
}

static void on_change(void *ud, kr_change what, const char *id)
{
    watch *w = ud;
    if (what == KR_CH_PAIR_REQUEST && id) {
        kr_ui_pair_dialog(w->ui, w->parent, id);
    } else if (what == KR_CH_DAEMON && kr_model_daemon_up(kr_ui_model(w->ui))) {
        show_pending(w); /* asked before we were here */
    } else if (what == KR_CH_DEVICES) {
        /* A question that was answered somewhere else (the phone gave up, the
         * CLI accepted) goes away. */
        GList *copy = g_list_copy(*kr_ui_pair_dialogs(w->ui));
        for (GList *l = copy; l; l = l->next) {
            const kr_dev *d = kr_model_find(kr_ui_model(w->ui), g_object_get_data(l->data, ID_KEY));
            if (!d || strcmp(d->state, "requested_by_peer") != 0)
                gtk_widget_destroy(l->data);
        }
        g_list_free(copy);
    }
}

static void parent_gone(gpointer data, GObject *where)
{
    watch *w = data;
    (void)where;
    w->parent = NULL;
}

void kr_ui_watch_pairing(kr_ui *ui, GtkWindow *parent)
{
    watch *w = g_new0(watch, 1);
    kr_ui_own(ui, w);
    w->ui = ui;
    w->parent = parent;
    if (parent)
        g_object_weak_ref(G_OBJECT(parent), parent_gone, w);
    kr_ui_listen(ui, on_change, w);
    if (kr_model_daemon_up(kr_ui_model(ui)))
        show_pending(w);
}
