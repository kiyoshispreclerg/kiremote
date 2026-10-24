/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The settings screen. It knows no setting by name: the daemon sends the
 * list (config-schema: type, texts, choices, which plugin it belongs to) and
 * the values (config-get), and every change goes back as config-set. A new
 * option in src/schema.c shows up here, and in the KiDesktop tab, by itself. */
#include <string.h>

#include "i18n.h"
#include "krui_private.h"

typedef struct settings settings;

typedef struct {
    settings *s;
    char key[96];
    char type[16];
    char plugin[48];
    char map_prefix[48]; /* "runcommand." for a map */
    GtkWidget *input;    /* the check button, entry, combo or tree view */
    GtkWidget *shown_row[3]; /* label, input box, help: made insensitive together */
    GtkListStore *store;     /* map */
    GPtrArray *choices;      /* enum: the values, in the combo's order */
    char *shown;             /* the value as the daemon last said it */
} field;

struct settings {
    int refs;
    gboolean dead, updating, restart;
    kr_ui *ui;
    GtkWidget *root, *body, *notice, *message;
    GtkWidget *tabs; /* the notebook, or NULL */
    GPtrArray *fields;
    cJSON *schema, *values;
    guint listener;
};

static void settings_unref(settings *s)
{
    if (--s->refs > 0)
        return;
    cJSON_Delete(s->schema);
    cJSON_Delete(s->values);
    g_ptr_array_free(s->fields, TRUE);
    g_free(s);
}

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static GtkWindow *toplevel_of(GtkWidget *w)
{
    GtkWidget *t = gtk_widget_get_toplevel(w);
    return GTK_WIDGET_TOPLEVEL(t) ? GTK_WINDOW(t) : NULL;
}

/* ---- the message line ----------------------------------------------------------- */

static void show_message(settings *s, const char *text, gboolean error)
{
    char *m = g_markup_printf_escaped(error ? "<span foreground=\"#c01c28\">%s</span>" : "<i>%s</i>", text);
    gtk_label_set_markup(GTK_LABEL(s->message), m);
    g_free(m);
    gtk_widget_show(s->message);
}

static void refresh_message(settings *s)
{
    if (s->restart)
        show_message(s, _("Some changes take effect when kiremoted starts again."), FALSE);
    else
        gtk_widget_hide(s->message);
}

/* ---- talking to the daemon -------------------------------------------------------- */

typedef struct {
    settings *s;
    enum { R_SCHEMA, R_VALUES, R_SET } what;
} reply_ctx;

static void load_values(settings *s);
static void build(settings *s);
static void apply_values(settings *s);

static void on_reply(void *ud, const cJSON *reply)
{
    reply_ctx *c = ud;
    settings *s = c->s;
    if (!s->dead) {
        const char *err = reply ? kr_reply_error(reply) : NULL;
        if (c->what == R_SET) {
            if (reply && err) {
                show_message(s, err, TRUE);
                load_values(s); /* back to what is really set */
            } else if (reply && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "restart"))) {
                s->restart = TRUE;
                refresh_message(s);
            } else if (reply) {
                refresh_message(s);
            }
        } else if (reply && !err) {
            if (c->what == R_SCHEMA) {
                cJSON_Delete(s->schema);
                s->schema = cJSON_Duplicate(reply, true);
                build(s);
            } else {
                cJSON_Delete(s->values);
                s->values = cJSON_Duplicate(reply, true);
                apply_values(s);
            }
        }
    }
    settings_unref(s);
    g_free(c);
}

static void send(settings *s, cJSON *req, int what)
{
    reply_ctx *c = g_new0(reply_ctx, 1);
    c->s = s;
    c->what = what;
    s->refs++;
    kr_client_send(kr_ui_client(s->ui), req, on_reply, c);
}

static cJSON *simple(const char *type)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", type);
    return r;
}

static void load_values(settings *s)
{
    send(s, simple("config-get"), R_VALUES);
}

static void set_value(settings *s, const char *key, const char *value)
{
    cJSON *r = simple("config-set");
    cJSON_AddStringToObject(r, "key", key);
    cJSON_AddStringToObject(r, "value", value);
    send(s, r, R_SET);
}

/* ---- the fields ---------------------------------------------------------------------- */

static const char *value_of(settings *s, const char *key)
{
    return s->values ? str(cJSON_GetObjectItemCaseSensitive(s->values, "values"), key) : "";
}

static void entry_commit(field *f)
{
    if (f->s->updating)
        return;
    const char *now = gtk_entry_get_text(GTK_ENTRY(f->input));
    if (strcmp(now, f->shown ? f->shown : "") != 0) {
        g_free(f->shown);
        f->shown = g_strdup(now);
        set_value(f->s, f->key, now);
    }
}

static void entry_activated(GtkEntry *e, gpointer data)
{
    (void)e;
    entry_commit(data);
}

static gboolean entry_focus_out(GtkWidget *w, GdkEventFocus *ev, gpointer data)
{
    (void)w;
    (void)ev;
    entry_commit(data);
    return FALSE;
}

static void check_toggled(GtkToggleButton *b, gpointer data)
{
    field *f = data;
    if (!f->s->updating)
        set_value(f->s, f->key, gtk_toggle_button_get_active(b) ? "true" : "false");
}

static void combo_changed(GtkComboBox *c, gpointer data)
{
    field *f = data;
    int i = gtk_combo_box_get_active(c);
    if (!f->s->updating && i >= 0 && (guint)i < f->choices->len)
        set_value(f->s, f->key, g_ptr_array_index(f->choices, (guint)i));
}

static void browse_clicked(GtkButton *b, gpointer data)
{
    field *f = data;
    gboolean folder = g_str_has_suffix(f->key, ".dir");
    GtkWidget *d = gtk_file_chooser_dialog_new(folder ? _("Choose a folder") : _("Choose a file"), toplevel_of(GTK_WIDGET(b)),
                                               folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
                                               GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (path) {
            gtk_entry_set_text(GTK_ENTRY(f->input), path);
            entry_commit(f);
            g_free(path);
        }
    }
    gtk_widget_destroy(d);
}

/* map: "Name = command" lines */
static void command_edited(GtkCellRendererText *r, gchar *path, gchar *text, gpointer data)
{
    field *f = data;
    (void)r;
    GtkTreeIter it;
    char *name = NULL;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(f->store), &it, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(f->store), &it, 0, &name, -1);
    char *key = g_strconcat(f->map_prefix, name, NULL);
    set_value(f->s, key, text); /* empty removes it */
    g_free(key);
    g_free(name);
}

static void map_add_clicked(GtkButton *b, gpointer data)
{
    field *f = data;
    GtkWidget *d = gtk_dialog_new_with_buttons(_("New command"), toplevel_of(GTK_WIDGET(b)), GTK_DIALOG_MODAL,
                                               GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 8);
    GtkWidget *name = gtk_entry_new(), *cmd = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(cmd), TRUE);
    gtk_table_attach(GTK_TABLE(table), gtk_label_new(_("Name")), 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), name, 1, 2, 0, 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), gtk_label_new(_("Command")), 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), cmd, 1, 2, 1, 2, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    gtk_container_set_border_width(GTK_CONTAINER(table), 8);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), table, TRUE, TRUE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(table);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT && *gtk_entry_get_text(GTK_ENTRY(name)) &&
        *gtk_entry_get_text(GTK_ENTRY(cmd))) {
        char *key = g_strconcat(f->map_prefix, gtk_entry_get_text(GTK_ENTRY(name)), NULL);
        set_value(f->s, key, gtk_entry_get_text(GTK_ENTRY(cmd)));
        g_free(key);
    }
    gtk_widget_destroy(d);
}

static void map_remove_clicked(GtkButton *b, gpointer data)
{
    field *f = data;
    (void)b;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(f->input));
    GtkTreeIter it;
    char *name = NULL;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(f->store), &it, 0, &name, -1);
    char *key = g_strconcat(f->map_prefix, name, NULL);
    set_value(f->s, key, "");
    g_free(key);
    g_free(name);
}

static GtkWidget *small_help(const char *text)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<small>%s</small>", text);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_selectable(GTK_LABEL(l), FALSE);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0);
    gtk_widget_set_size_request(l, 340, -1);
    return l;
}

static field *field_new(settings *s, const cJSON *def)
{
    field *f = g_new0(field, 1);
    f->s = s;
    g_snprintf(f->key, sizeof f->key, "%s", str(def, "key"));
    g_snprintf(f->type, sizeof f->type, "%s", str(def, "type"));
    g_snprintf(f->plugin, sizeof f->plugin, "%s", str(def, "plugin"));
    g_ptr_array_add(s->fields, f);
    return f;
}

/* Adds the field's widgets to table at row; returns the next row. */
static int add_field(settings *s, GtkWidget *table, int row, const cJSON *def)
{
    field *f = field_new(s, def);
    const char *label = str(def, "label"), *help = str(def, "help");
    GtkWidget *help_w = *help ? small_help(help) : NULL;

    if (strcmp(f->type, "bool") == 0) {
        f->input = gtk_check_button_new_with_label(label);
        g_signal_connect(f->input, "toggled", G_CALLBACK(check_toggled), f);
        gtk_table_attach(GTK_TABLE(table), f->input, 0, 3, (guint)row, (guint)row + 1, GTK_FILL, 0, 0, 0);
        f->shown_row[0] = f->input;
        if (help_w) {
            GtkWidget *a = gtk_alignment_new(0, 0, 1, 1);
            gtk_alignment_set_padding(GTK_ALIGNMENT(a), 0, 0, 24, 0);
            gtk_container_add(GTK_CONTAINER(a), help_w);
            gtk_table_attach(GTK_TABLE(table), a, 0, 3, (guint)row + 1, (guint)row + 2, GTK_FILL, 0, 0, 0);
            f->shown_row[2] = a;
            return row + 2;
        }
        return row + 1;
    }

    if (strcmp(f->type, "note") == 0) { /* a paragraph the daemon writes: not an input */
        f->input = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(f->input), TRUE);
        gtk_label_set_selectable(GTK_LABEL(f->input), TRUE); /* the commands can be copied */
        gtk_misc_set_alignment(GTK_MISC(f->input), 0, 0);
        gtk_widget_set_size_request(f->input, 400, -1);
        GtkWidget *frame = gtk_frame_new(NULL);
        gtk_misc_set_padding(GTK_MISC(f->input), 8, 8);
        gtk_container_add(GTK_CONTAINER(frame), f->input);
        gtk_table_attach(GTK_TABLE(table), frame, 0, 3, (guint)row, (guint)row + 1, GTK_FILL | GTK_EXPAND, 0, 0, 6);
        f->shown_row[0] = frame;
        return row + 1;
    }

    if (strcmp(f->type, "map") == 0) {
        g_snprintf(f->map_prefix, sizeof f->map_prefix, "%.*s.", (int)(strchr(f->key, '.') - f->key), f->key);
        GtkWidget *head = gtk_label_new(NULL);
        char *m = g_markup_printf_escaped("<b>%s</b>", label);
        gtk_label_set_markup(GTK_LABEL(head), m);
        g_free(m);
        gtk_misc_set_alignment(GTK_MISC(head), 0, 0.5);
        gtk_table_attach(GTK_TABLE(table), head, 0, 3, (guint)row, (guint)row + 1, GTK_FILL, 0, 0, 0);
        f->shown_row[0] = head;

        f->store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        f->input = gtk_tree_view_new_with_model(GTK_TREE_MODEL(f->store));
        gtk_tree_view_append_column(GTK_TREE_VIEW(f->input),
                                    gtk_tree_view_column_new_with_attributes(_("Name"), gtk_cell_renderer_text_new(), "text", 0, NULL));
        GtkCellRenderer *cell = gtk_cell_renderer_text_new();
        g_object_set(cell, "editable", TRUE, NULL);
        g_signal_connect(cell, "edited", G_CALLBACK(command_edited), f);
        gtk_tree_view_append_column(GTK_TREE_VIEW(f->input),
                                    gtk_tree_view_column_new_with_attributes(_("Command"), cell, "text", 1, NULL));
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(sw), f->input);
        gtk_widget_set_size_request(sw, 340, 120);
        gtk_table_attach(GTK_TABLE(table), sw, 0, 3, (guint)row + 1, (guint)row + 2, GTK_FILL | GTK_EXPAND, GTK_FILL | GTK_EXPAND, 0, 4);
        f->shown_row[1] = sw;

        GtkWidget *bar = gtk_hbox_new(FALSE, 6);
        GtkWidget *add = gtk_button_new_from_stock(GTK_STOCK_ADD), *rm = gtk_button_new_from_stock(GTK_STOCK_REMOVE);
        g_signal_connect(add, "clicked", G_CALLBACK(map_add_clicked), f);
        g_signal_connect(rm, "clicked", G_CALLBACK(map_remove_clicked), f);
        gtk_box_pack_start(GTK_BOX(bar), add, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bar), rm, FALSE, FALSE, 0);
        gtk_table_attach(GTK_TABLE(table), bar, 0, 3, (guint)row + 2, (guint)row + 3, GTK_FILL, 0, 0, 0);
        f->shown_row[2] = bar;
        if (help_w)
            gtk_table_attach(GTK_TABLE(table), help_w, 0, 3, (guint)row + 3, (guint)row + 4, GTK_FILL, 0, 0, 0);
        return row + (help_w ? 4 : 3);
    }

    GtkWidget *l = gtk_label_new(label);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_table_attach(GTK_TABLE(table), l, 0, 1, (guint)row, (guint)row + 1, GTK_FILL, 0, 0, 0);
    f->shown_row[0] = l;

    GtkWidget *box = gtk_hbox_new(FALSE, 6);
    if (strcmp(f->type, "enum") == 0) {
        f->input = gtk_combo_box_new_text();
        f->choices = g_ptr_array_new_with_free_func(g_free);
        const cJSON *vals = cJSON_GetObjectItemCaseSensitive(def, "choices"), *labels = cJSON_GetObjectItemCaseSensitive(def, "choice_labels"), *v;
        int i = 0;
        cJSON_ArrayForEach(v, vals)
        {
            const cJSON *lb = cJSON_GetArrayItem(labels, i++);
            g_ptr_array_add(f->choices, g_strdup(cJSON_IsString(v) ? v->valuestring : ""));
            gtk_combo_box_append_text(GTK_COMBO_BOX(f->input), cJSON_IsString(lb) ? lb->valuestring : v->valuestring);
        }
        g_signal_connect(f->input, "changed", G_CALLBACK(combo_changed), f);
        gtk_box_pack_start(GTK_BOX(box), f->input, FALSE, FALSE, 0);
    } else {
        f->input = gtk_entry_new();
        gtk_widget_set_size_request(f->input, 220, -1);
        g_signal_connect(f->input, "activate", G_CALLBACK(entry_activated), f);
        g_signal_connect(f->input, "focus-out-event", G_CALLBACK(entry_focus_out), f);
        gtk_box_pack_start(GTK_BOX(box), f->input, TRUE, TRUE, 0);
        if (strcmp(f->type, "path") == 0) {
            GtkWidget *b = gtk_button_new_with_label(_("Browse..."));
            g_signal_connect(b, "clicked", G_CALLBACK(browse_clicked), f);
            gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
        }
    }
    if (*help)
        gtk_widget_set_tooltip_text(f->input, help);
    gtk_table_attach(GTK_TABLE(table), box, 1, 3, (guint)row, (guint)row + 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    f->shown_row[1] = box;
    if (help_w) {
        gtk_table_attach(GTK_TABLE(table), help_w, 1, 3, (guint)row + 1, (guint)row + 2, GTK_FILL, 0, 0, 0);
        f->shown_row[2] = help_w;
        return row + 2;
    }
    return row + 1;
}

/* ---- values into the widgets ------------------------------------------------------------ */

static void apply_values(settings *s)
{
    if (!s->values || !s->tabs)
        return;
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(s->values, "values");
    s->updating = TRUE;
    for (guint i = 0; i < s->fields->len; i++) {
        field *f = g_ptr_array_index(s->fields, i);
        if (strcmp(f->type, "map") == 0) {
            gtk_list_store_clear(f->store);
            size_t plen = strlen(f->map_prefix);
            for (const cJSON *v = values ? values->child : NULL; v; v = v->next)
                if (v->string && strncmp(v->string, f->map_prefix, plen) == 0 && cJSON_IsString(v)) {
                    GtkTreeIter it;
                    gtk_list_store_append(f->store, &it);
                    gtk_list_store_set(f->store, &it, 0, v->string + plen, 1, v->valuestring, -1);
                }
            continue;
        }
        const char *val = value_of(s, f->key);
        if (strcmp(f->type, "note") == 0) {
            if (strcmp(gtk_label_get_text(GTK_LABEL(f->input)), val) != 0) /* keeps a selection */
                gtk_label_set_text(GTK_LABEL(f->input), val);
            continue;
        }
        if (strcmp(f->type, "bool") == 0) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f->input), strcmp(val, "true") == 0);
        } else if (strcmp(f->type, "enum") == 0) {
            for (guint k = 0; k < f->choices->len; k++)
                if (strcmp(g_ptr_array_index(f->choices, k), val) == 0)
                    gtk_combo_box_set_active(GTK_COMBO_BOX(f->input), (gint)k);
        } else {
            /* Don't overwrite what is being typed in another field. */
            if (!GTK_WIDGET_HAS_FOCUS(f->input))
                gtk_entry_set_text(GTK_ENTRY(f->input), val);
            g_free(f->shown);
            f->shown = g_strdup(gtk_entry_get_text(GTK_ENTRY(f->input)));
        }
    }
    /* What belongs to a plugin that is off can't be used. */
    for (guint i = 0; i < s->fields->len; i++) {
        field *f = g_ptr_array_index(s->fields, i);
        if (!f->plugin[0])
            continue;
        char *k = g_strconcat("plugins.", f->plugin, NULL);
        gboolean on = strcmp(value_of(s, k), "false") != 0;
        g_free(k);
        for (int w = 0; w < 3; w++)
            if (f->shown_row[w])
                gtk_widget_set_sensitive(f->shown_row[w], on);
    }
    s->updating = FALSE;
}

/* ---- the pages ----------------------------------------------------------------------------- */

static void clear_pages(settings *s)
{
    if (s->tabs) {
        gtk_widget_destroy(s->tabs);
        s->tabs = NULL;
    }
    for (guint i = 0; i < s->fields->len; i++) {
        field *f = g_ptr_array_index(s->fields, i);
        g_free(f->shown);
        if (f->choices)
            g_ptr_array_free(f->choices, TRUE);
        if (f->store)
            g_object_unref(f->store);
    }
    g_ptr_array_set_size(s->fields, 0); /* frees the fields */
}

static void build(settings *s)
{
    clear_pages(s);
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(s->schema, "groups");
    const cJSON *settings_l = cJSON_GetObjectItemCaseSensitive(s->schema, "settings");
    s->tabs = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(s->tabs), TRUE);
    const cJSON *g, *def;
    cJSON_ArrayForEach(g, groups)
    {
        GtkWidget *table = gtk_table_new(1, 3, FALSE);
        gtk_table_set_row_spacings(GTK_TABLE(table), 4);
        gtk_table_set_col_spacings(GTK_TABLE(table), 10);
        gtk_container_set_border_width(GTK_CONTAINER(table), 12);
        int row = 0;
        cJSON_ArrayForEach(def, settings_l) if (strcmp(str(def, "group"), str(g, "id")) == 0) row = add_field(s, table, row, def) + 0;
        if (!row) {
            gtk_widget_destroy(table);
            continue;
        }
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(sw), table);
        gtk_viewport_set_shadow_type(GTK_VIEWPORT(GTK_BIN(sw)->child), GTK_SHADOW_NONE);
        gtk_notebook_append_page(GTK_NOTEBOOK(s->tabs), sw, gtk_label_new(str(g, "label")));
    }
    gtk_box_pack_start(GTK_BOX(s->body), s->tabs, TRUE, TRUE, 0);
    gtk_widget_show_all(s->tabs);
    gtk_widget_hide(s->notice);
    if (s->values)
        apply_values(s);
    else
        load_values(s);
}

static void reload(settings *s)
{
    kr_ui *ui = s->ui;
    if (!kr_model_daemon_up(kr_ui_model(ui))) {
        clear_pages(s);
        gtk_widget_show(s->notice);
        return;
    }
    send(s, simple("config-schema"), R_SCHEMA);
    load_values(s);
}

/* The notes say how things stand (is the module loaded?), which changes
 * outside the settings: ask again now and then while the window is there. */
static gboolean refresh_notes(gpointer data)
{
    settings *s = data;
    if (s->dead) {
        settings_unref(s);
        return FALSE;
    }
    if (s->tabs && GTK_WIDGET_MAPPED(s->root))
        for (guint i = 0; i < s->fields->len; i++)
            if (strcmp(((field *)g_ptr_array_index(s->fields, i))->type, "note") == 0) {
                load_values(s);
                break;
            }
    return TRUE;
}

static void on_change(void *ud, kr_change what, const char *id)
{
    settings *s = ud;
    (void)id;
    if (what == KR_CH_DAEMON) {
        cJSON_Delete(s->schema);
        cJSON_Delete(s->values);
        s->schema = s->values = NULL;
        s->restart = FALSE;
        refresh_message(s);
        reload(s);
    } else if (what == KR_CH_CONFIG && s->tabs) {
        load_values(s);
    }
}

static void root_destroyed(GtkWidget *w, gpointer data)
{
    settings *s = data;
    (void)w;
    kr_ui_unlisten(s->ui, s->listener);
    s->dead = TRUE;
    s->tabs = NULL; /* destroyed with the root */
    for (guint i = 0; i < s->fields->len; i++) {
        field *f = g_ptr_array_index(s->fields, i);
        g_free(f->shown);
        if (f->choices)
            g_ptr_array_free(f->choices, TRUE);
        if (f->store)
            g_object_unref(f->store);
    }
    g_ptr_array_set_size(s->fields, 0);
    settings_unref(s);
}

GtkWidget *kr_ui_settings_new(kr_ui *ui)
{
    settings *s = g_new0(settings, 1);
    s->refs = 1;
    s->ui = ui;
    s->fields = g_ptr_array_new_with_free_func(g_free);
    s->root = gtk_vbox_new(FALSE, 6);
    s->body = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->root), s->body, TRUE, TRUE, 0);

    s->notice = gtk_label_new(_("Kiremote is not running.\nStart kiremoted to change its settings."));
    gtk_label_set_justify(GTK_LABEL(s->notice), GTK_JUSTIFY_CENTER);
    gtk_misc_set_padding(GTK_MISC(s->notice), 12, 16);
    gtk_box_pack_start(GTK_BOX(s->body), s->notice, FALSE, FALSE, 0);

    s->message = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(s->message), TRUE);
    gtk_misc_set_alignment(GTK_MISC(s->message), 0, 0.5);
    gtk_widget_set_no_show_all(s->message, TRUE);
    gtk_box_pack_start(GTK_BOX(s->root), s->message, FALSE, FALSE, 4);

    s->listener = kr_ui_listen(ui, on_change, s);
    s->refs++;
    g_timeout_add_seconds(3, refresh_notes, s);
    g_signal_connect(s->root, "destroy", G_CALLBACK(root_destroyed), s);
    gtk_widget_show_all(s->root);
    gtk_widget_hide(s->message);
    reload(s);
    return s->root;
}
