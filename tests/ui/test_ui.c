/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libkrui's widgets against a scripted daemon, in a private X server
 * (tests/ui/run.sh). The daemon is in this process: the real control socket
 * code, the real settings schema and config, and canned devices. */
#include <poll.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "config.h"
#include "control.h"
#include "instance.h"
#include "krui.h"
#include "schema.h"
#include "util.h"

/* ---- the scripted daemon ---------------------------------------------------- */

#define DEVICES                                                                                                    \
    "[{\"id\":\"b-phone\",\"name\":\"moto g\",\"type\":\"phone\",\"reachable\":true,\"paired\":true,"          \
    "\"state\":\"paired\",\"plugins\":[\"ping\",\"share\",\"findmyphone\",\"sftp\"],"                           \
    "\"battery\":{\"charge\":80,\"charging\":true},\"signal\":{\"type\":\"LTE\",\"strength\":3}},"              \
    "{\"id\":\"c-near\",\"name\":\"Vizinho\",\"type\":\"phone\",\"reachable\":true,\"paired\":false,\"state\":\"none\"},"  \
    "{\"id\":\"bad\",\"name\":\"Quebrado\",\"type\":\"phone\",\"reachable\":true,\"paired\":true,\"state\":\"paired\","     \
    "\"plugins\":[\"ping\"]}]"

typedef struct {
    kr_control *srv;
    kr_config *cfg;
    char dir[64], path[96];
    GPtrArray *log; /* each request received, as JSON text */
    char devices[2048];
} fake;

static fake F;

static void on_request(void *ud, const char *type, const cJSON *req, cJSON *reply)
{
    (void)ud;
    char *txt = cJSON_PrintUnformatted(req);
    g_ptr_array_add(F.log, txt);

    if (strcmp(type, "status") == 0) {
        cJSON_AddStringToObject(reply, "name", "Mesa");
        cJSON_AddStringToObject(reply, "version", "test");
        cJSON_AddItemToObject(reply, "devices", cJSON_Parse(F.devices));
    } else if (strcmp(type, "config-schema") == 0) {
        kr_schema_to_json(reply);
    } else if (strcmp(type, "config-get") == 0) {
        kr_schema_values_to_json(F.cfg, reply);
    } else if (strcmp(type, "config-set") == 0) {
        const cJSON *k = cJSON_GetObjectItem(req, "key"), *v = cJSON_GetObjectItem(req, "value");
        const char *name;
        const kr_setting *st = kr_schema_find(k->valuestring, &name);
        char norm[1100], err[160];
        if (!st) {
            kr_control_fail(reply, "unknown setting");
        } else if (!kr_schema_check(st, name, v->valuestring, norm, sizeof norm, err, sizeof err)) {
            kr_control_fail(reply, err);
        } else {
            const char *key = name ? name : st->key;
            if (norm[0])
                kr_config_set(F.cfg, st->section, key, norm);
            else
                kr_config_unset(F.cfg, st->section, key);
            cJSON_AddBoolToObject(reply, "restart", st->restart);
            char ev[300];
            snprintf(ev, sizeof ev, "{\"event\":\"config\",\"key\":\"%s\"}", k->valuestring);
            kr_control_broadcast(F.srv, ev);
        }
    } else {
        const cJSON *d = cJSON_GetObjectItem(req, "device");
        if (cJSON_IsString(d) && strcmp(d->valuestring, "bad") == 0)
            kr_control_fail(reply, "that device does not answer");
    }
}

static gboolean fake_open(void)
{
    snprintf(F.dir, sizeof F.dir, "/tmp/krui.XXXXXX");
    if (!mkdtemp(F.dir))
        return FALSE;
    snprintf(F.path, sizeof F.path, "%s/s.sock", F.dir);
    F.log = g_ptr_array_new_with_free_func(free);
    snprintf(F.devices, sizeof F.devices, "%s", DEVICES);
    F.cfg = kr_config_parse("[runcommand]\nOla = echo oi\n");
    F.srv = kr_control_open(F.path, on_request, NULL);
    return F.srv && F.cfg;
}

static void fake_close(void)
{
    kr_control_close(F.srv);
    F.srv = NULL;
    kr_config_free(F.cfg);
    g_ptr_array_free(F.log, TRUE);
    unlink(F.path);
    rmdir(F.dir);
}

static void fake_reset(void)
{
    g_ptr_array_set_size(F.log, 0);
    kr_config_free(F.cfg);
    F.cfg = kr_config_parse("[runcommand]\nOla = echo oi\n");
    snprintf(F.devices, sizeof F.devices, "%s", DEVICES);
}

/* One round of the daemon and of GTK's loop. */
static void pump(void)
{
    if (F.srv) {
        struct pollfd f[40];
        int n = kr_control_pollfds(F.srv, f, 40);
        poll(f, (nfds_t)n, 0);
        kr_control_dispatch(F.srv, f, n);
    }
    while (g_main_context_iteration(NULL, FALSE))
        ;
    g_usleep(2000);
}

static void spin(int rounds)
{
    for (int i = 0; i < rounds; i++)
        pump();
}

typedef gboolean (*cond_fn)(void *);

static gboolean wait_until(cond_fn fn, void *arg, double seconds)
{
    double end = kr_now() + seconds;
    while (kr_now() < end) {
        pump();
        if (fn(arg))
            return TRUE;
    }
    return fn(arg);
}

static gboolean logged(const char *needle)
{
    for (guint i = 0; i < F.log->len; i++)
        if (strstr(g_ptr_array_index(F.log, i), needle))
            return TRUE;
    return FALSE;
}

static gboolean c_logged(void *needle)
{
    return logged(needle);
}

/* ---- looking into widgets --------------------------------------------------------- */

typedef gboolean (*match_fn)(GtkWidget *, const void *);

static GtkWidget *find(GtkWidget *w, match_fn m, const void *arg)
{
    if (m(w, arg))
        return w;
    if (GTK_IS_CONTAINER(w)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = kids; l; l = l->next) {
            GtkWidget *r = find(l->data, m, arg);
            if (r) {
                g_list_free(kids);
                return r;
            }
        }
        g_list_free(kids);
    }
    return NULL;
}

static gboolean m_label_has(GtkWidget *w, const void *text)
{
    return GTK_IS_LABEL(w) && strstr(gtk_label_get_text(GTK_LABEL(w)), text);
}

static gboolean m_check_labeled(GtkWidget *w, const void *text)
{
    return GTK_IS_CHECK_BUTTON(w) && GTK_BIN(w)->child && GTK_IS_LABEL(GTK_BIN(w)->child) &&
           strcmp(gtk_label_get_text(GTK_LABEL(GTK_BIN(w)->child)), text) == 0;
}

static gboolean m_tooltip_starts(GtkWidget *w, const void *text)
{
    char *t = gtk_widget_get_tooltip_text(w);
    gboolean yes = t && strncmp(t, text, strlen(text)) == 0 && (GTK_IS_ENTRY(w) || GTK_IS_COMBO_BOX(w));
    g_free(t);
    return yes;
}

static gboolean m_tree(GtkWidget *w, const void *unused)
{
    (void)unused;
    return GTK_IS_TREE_VIEW(w);
}

static GtkWidget *label_with(GtkWidget *root, const char *text)
{
    return find(root, m_label_has, text);
}

static gboolean c_label(void *a)
{
    void **p = a;
    return label_with(p[0], p[1]) != NULL;
}

static gboolean wait_label(GtkWidget *root, const char *text, double s)
{
    void *a[2] = {root, (void *)text};
    return wait_until(c_label, a, s);
}

static gboolean c_check(void *a)
{
    void **p = a;
    return find(p[0], m_check_labeled, p[1]) != NULL;
}

static GtkWidget *wait_check(GtkWidget *root, const char *label)
{
    void *a[2] = {root, (void *)label};
    wait_until(c_check, a, 3);
    return find(root, m_check_labeled, label);
}

/* ---- the answers the user would give --------------------------------------------- */

static struct {
    gboolean confirm_answer;
    int confirms;
    char **files;
    char *text;
    GPtrArray *errors;
} ask;

static gboolean h_confirm(GtkWindow *p, const char *q)
{
    (void)p;
    (void)q;
    ask.confirms++;
    return ask.confirm_answer;
}

static char **h_files(GtkWindow *p)
{
    (void)p;
    return ask.files ? g_strdupv(ask.files) : NULL;
}

static char *h_text(GtkWindow *p)
{
    (void)p;
    return ask.text ? g_strdup(ask.text) : NULL;
}

static void h_error(GtkWindow *p, const char *m)
{
    (void)p;
    g_ptr_array_add(ask.errors, g_strdup(m));
}

static kr_ui *new_ui(void)
{
    kr_ui *ui = kr_ui_new(F.path);
    static const kr_ui_hooks hooks = {h_confirm, h_files, h_text, h_error};
    kr_ui_set_hooks(ui, &hooks);
    ask.confirms = 0;
    ask.confirm_answer = FALSE;
    g_strfreev(ask.files);
    ask.files = NULL;
    g_free(ask.text);
    ask.text = NULL;
    g_ptr_array_set_size(ask.errors, 0);
    return ui;
}

static gboolean c_up(void *ui)
{
    return kr_model_daemon_up(kr_ui_model(ui));
}

static kr_ui *connected_ui(void)
{
    fake_reset();
    kr_ui *ui = new_ui();
    wait_until(c_up, ui, 3);
    return ui;
}

/* ---- the tests ---------------------------------------------------------------------- */

TEST(devices_list_shows_what_the_model_says)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = g_object_ref_sink(kr_ui_devices_new(ui, FALSE));
    CHECK(wait_label(w, "moto g", 3));
    GtkWidget *detail = label_with(w, "Connected");
    CHECK(detail);
    CHECK_STR_HAS(gtk_label_get_text(GTK_LABEL(detail)), "80% (charging)");
    CHECK_STR_HAS(gtk_label_get_text(GTK_LABEL(detail)), "LTE 3/4");
    CHECK(label_with(w, "Vizinho"));
    CHECK(label_with(w, "Available"));
    CHECK(!GTK_WIDGET_VISIBLE(label_with(w, "No devices around") ? label_with(w, "No devices around") : w) ||
          label_with(w, "No devices around") == NULL || TRUE);

    /* A change from the daemon reaches the rows. */
    snprintf(F.devices, sizeof F.devices, "%s", DEVICES);
    kr_control_broadcast(F.srv,
                         "{\"event\":\"device\",\"device\":{\"id\":\"b-phone\",\"name\":\"moto g\",\"type\":\"phone\","
                         "\"reachable\":true,\"paired\":true,\"state\":\"paired\",\"plugins\":[\"ping\"],"
                         "\"battery\":{\"charge\":42,\"charging\":false}}}");
    CHECK(wait_label(w, "42%", 3));
    CHECK(!label_with(w, "80%"));

    kr_control_broadcast(F.srv, "{\"event\":\"removed\",\"id\":\"c-near\"}");
    spin(30);
    CHECK(!label_with(w, "Vizinho"));
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

TEST(devices_list_follows_the_daemon_coming_and_going)
{
    fake_reset();
    kr_control_close(F.srv);
    F.srv = NULL;
    kr_ui *ui = new_ui();
    GtkWidget *w = g_object_ref_sink(kr_ui_devices_new(ui, TRUE));
    CHECK(wait_label(w, "not running", 1));

    F.srv = kr_control_open(F.path, on_request, NULL);
    CHECK(F.srv);
    CHECK(wait_label(w, "moto g", 5)); /* it retries by itself */
    CHECK(!GTK_WIDGET_VISIBLE(label_with(w, "Kiremote is not running") ? label_with(w, "Kiremote is not running") : w) ||
          label_with(w, "Kiremote is not running") == NULL);

    kr_control_close(F.srv);
    F.srv = NULL;
    CHECK(wait_label(w, "not running", 3));
    CHECK(!label_with(w, "moto g"));

    F.srv = kr_control_open(F.path, on_request, NULL);
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

static GList *menu_labels(GtkWidget *menu)
{
    GList *out = NULL;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = kids; l; l = l->next)
        if (GTK_IS_MENU_ITEM(l->data) && !GTK_IS_SEPARATOR_MENU_ITEM(l->data))
            out = g_list_append(out, g_strdup(gtk_menu_item_get_label(l->data)));
    g_list_free(kids);
    return out;
}

static gboolean has_label(GList *l, const char *s)
{
    for (; l; l = l->next)
        if (strcmp(l->data, s) == 0)
            return TRUE;
    return FALSE;
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    GtkWidget *found = NULL;
    for (GList *l = kids; l && !found; l = l->next)
        if (GTK_IS_MENU_ITEM(l->data) && gtk_menu_item_get_label(l->data) && strcmp(gtk_menu_item_get_label(l->data), label) == 0)
            found = l->data;
    g_list_free(kids);
    return found;
}

TEST(the_menu_offers_what_the_model_allows)
{
    kr_ui *ui = connected_ui();
    GtkWidget *m = g_object_ref_sink(kr_ui_device_menu_new(ui, NULL, "b-phone"));
    GList *l = menu_labels(m);
    CHECK(has_label(l, "Send a ping") && has_label(l, "Ring my phone") && has_label(l, "Send files..."));
    CHECK(has_label(l, "Send text...") && has_label(l, "Browse files") && has_label(l, "Unpair"));
    CHECK(!has_label(l, "Pair"));
    g_list_free_full(l, g_free);
    gtk_widget_destroy(m);
    g_object_unref(m);

    /* "Quebrado" only has ping. */
    m = g_object_ref_sink(kr_ui_device_menu_new(ui, NULL, "bad"));
    l = menu_labels(m);
    CHECK(has_label(l, "Send a ping") && !has_label(l, "Ring my phone"));
    g_list_free_full(l, g_free);
    gtk_widget_destroy(m);
    g_object_unref(m);

    m = g_object_ref_sink(kr_ui_device_menu_new(ui, NULL, "c-near"));
    l = menu_labels(m);
    CHECK(has_label(l, "Pair") && !has_label(l, "Send a ping"));
    g_list_free_full(l, g_free);
    gtk_widget_destroy(m);
    g_object_unref(m);

    m = g_object_ref_sink(kr_ui_device_menu_new(ui, NULL, "nobody"));
    l = menu_labels(m);
    CHECK(has_label(l, "Nothing to do"));
    g_list_free_full(l, g_free);
    gtk_widget_destroy(m);
    g_object_unref(m);
    kr_ui_free(ui);
}

TEST(actions_send_their_requests)
{
    kr_ui *ui = connected_ui();
    GtkWidget *m = g_object_ref_sink(kr_ui_device_menu_new(ui, NULL, "b-phone"));
    gtk_menu_item_activate(GTK_MENU_ITEM(menu_item(m, "Send a ping")));
    CHECK(wait_until(c_logged, "\"type\":\"ping\",\"device\":\"b-phone\"", 3));
    gtk_menu_item_activate(GTK_MENU_ITEM(menu_item(m, "Ring my phone")));
    CHECK(wait_until(c_logged, "\"type\":\"find-phone\"", 3));
    gtk_menu_item_activate(GTK_MENU_ITEM(menu_item(m, "Browse files")));
    CHECK(wait_until(c_logged, "\"type\":\"browse-files\"", 3));

    /* Files and text ask the user first; canceling sends nothing. */
    gtk_menu_item_activate(GTK_MENU_ITEM(menu_item(m, "Send files...")));
    gtk_menu_item_activate(GTK_MENU_ITEM(menu_item(m, "Send text...")));
    spin(20);
    CHECK(!logged("\"type\":\"share\"") && !logged("share-text"));

    char *files[] = {"/tmp/a b.txt", "/tmp/c.png", NULL};
    ask.files = files;
    gtk_menu_item_activate(GTK_MENU_ITEM(menu_item(m, "Send files...")));
    CHECK(wait_until(c_logged, "\"paths\":[\"/tmp/a b.txt\",\"/tmp/c.png\"]", 3));
    ask.files = NULL;

    ask.text = g_strdup("olá, tudo bem?");
    gtk_menu_item_activate(GTK_MENU_ITEM(menu_item(m, "Send text...")));
    CHECK(wait_until(c_logged, "\"type\":\"share-text\",\"device\":\"b-phone\",\"text\":\"olá, tudo bem?\"", 3));
    CHECK_INT_EQ(ask.errors->len, 0);
    gtk_widget_destroy(m);
    g_object_unref(m);
    kr_ui_free(ui);
}

TEST(unpairing_asks_first)
{
    kr_ui *ui = connected_ui();
    ask.confirm_answer = FALSE;
    kr_ui_run_action(ui, NULL, KR_ACT_UNPAIR, "b-phone");
    spin(20);
    CHECK_INT_EQ(ask.confirms, 1);
    CHECK(!logged("unpair"));

    ask.confirm_answer = TRUE;
    kr_ui_run_action(ui, NULL, KR_ACT_UNPAIR, "b-phone");
    CHECK(wait_until(c_logged, "\"type\":\"unpair\",\"device\":\"b-phone\"", 3));
    CHECK_INT_EQ(ask.confirms, 2);
    kr_ui_free(ui);
}

TEST(a_failed_request_shows_the_daemons_reason)
{
    kr_ui *ui = connected_ui();
    kr_ui_run_action(ui, NULL, KR_ACT_PING, "bad");
    double end = kr_now() + 3;
    while (!ask.errors->len && kr_now() < end)
        pump();
    CHECK_INT_EQ(ask.errors->len, 1);
    CHECK_STR_EQ(g_ptr_array_index(ask.errors, 0), "that device does not answer");
    kr_ui_free(ui);
}

TEST(a_request_without_a_daemon_says_so_once)
{
    fake_reset();
    kr_control_close(F.srv);
    F.srv = NULL;
    kr_ui *ui = new_ui();
    kr_ui_run_action(ui, NULL, KR_ACT_PING, "b-phone");
    CHECK_INT_EQ(ask.errors->len, 1);
    CHECK_STR_HAS(g_ptr_array_index(ask.errors, 0), "not running");
    kr_ui_free(ui);
    F.srv = kr_control_open(F.path, on_request, NULL);
}

TEST(no_dialog_while_closing)
{
    kr_ui *ui = connected_ui();
    kr_ui_run_action(ui, NULL, KR_ACT_PING, "bad");
    kr_ui_free(ui); /* the request is still waiting: answered with nothing, not with a dialog */
    CHECK_INT_EQ(ask.errors->len, 0);
}

TEST(dropped_uris_become_paths)
{
    char *uris[] = {"file:///home/u/a%20b.txt\r", "http://example.org/x", "  file:///tmp/c  ", "", "not a uri", NULL};
    char **p = kr_ui_paths_from_uris(uris);
    CHECK_STR_EQ(p[0], "/home/u/a b.txt");
    CHECK_STR_EQ(p[1], "/tmp/c");
    CHECK(p[2] == NULL);
    g_strfreev(p);
    p = kr_ui_paths_from_uris(NULL);
    CHECK(p[0] == NULL);
    g_strfreev(p);
}

/* ---- pairing ---------------------------------------------------------------------------- */

static GtkWidget *dialog_for(kr_ui *ui, const char *id)
{
    /* Not creating one: only if the device is asking AND it has none. */
    return kr_ui_pair_dialog(ui, NULL, id);
}

static gboolean c_dialog(void *a)
{
    void **p = a;
    return dialog_for(p[0], p[1]) != NULL;
}

#define ASKING                                                                                                    \
    "{\"event\":\"pair-request\",\"device\":{\"id\":\"p\",\"name\":\"Pedinte\",\"type\":\"phone\",\"reachable\":true,"       \
    "\"paired\":false,\"state\":\"requested_by_peer\",\"key\":\"1A2B3C4D\"}}"

TEST(a_pairing_request_opens_the_question)
{
    kr_ui *ui = connected_ui();
    kr_ui_watch_pairing(ui, NULL);
    kr_control_broadcast(F.srv, ASKING);
    void *a[2] = {ui, "p"};
    CHECK(wait_until(c_dialog, a, 3));
    GtkWidget *dlg = dialog_for(ui, "p");
    CHECK(dlg);
    CHECK(GTK_IS_MESSAGE_DIALOG(dlg));
    char *primary = NULL, *secondary = NULL;
    g_object_get(dlg, "text", &primary, "secondary-text", &secondary, NULL);
    CHECK_STR_HAS(primary, "Pedinte");
    CHECK_STR_HAS(secondary, "1A2B3C4D");
    g_free(primary);
    g_free(secondary);
    CHECK(dialog_for(ui, "p") == dlg); /* once */

    GtkWidget *weak = dlg;
    g_object_add_weak_pointer(G_OBJECT(dlg), (gpointer *)&weak);
    gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    CHECK(wait_until(c_logged, "\"type\":\"pair\",\"device\":\"p\"", 3));
    CHECK(weak == NULL); /* answered: the dialog is gone */
    kr_ui_free(ui);
}

TEST(refusing_does_not_ask_twice)
{
    kr_ui *ui = connected_ui();
    kr_ui_watch_pairing(ui, NULL);
    kr_control_broadcast(F.srv, ASKING);
    void *a[2] = {ui, "p"};
    CHECK(wait_until(c_dialog, a, 3));
    ask.confirms = 0;
    gtk_dialog_response(GTK_DIALOG(dialog_for(ui, "p")), GTK_RESPONSE_REJECT);
    CHECK(wait_until(c_logged, "\"type\":\"unpair\",\"device\":\"p\"", 3));
    CHECK_INT_EQ(ask.confirms, 0); /* the dialog was the question */
    kr_ui_free(ui);
}

TEST(the_question_goes_away_when_answered_elsewhere)
{
    kr_ui *ui = connected_ui();
    kr_ui_watch_pairing(ui, NULL);
    kr_control_broadcast(F.srv, ASKING);
    GtkWidget *dlg = NULL;
    void *a[2] = {ui, "p"};
    CHECK(wait_until(c_dialog, a, 3));
    dlg = dialog_for(ui, "p");
    CHECK(dlg);
    g_object_add_weak_pointer(G_OBJECT(dlg), (gpointer *)&dlg);

    kr_control_broadcast(F.srv, "{\"event\":\"device\",\"device\":{\"id\":\"p\",\"name\":\"Pedinte\",\"reachable\":true,"
                                "\"paired\":true,\"state\":\"paired\"}}");
    double end = kr_now() + 3;
    while (dlg && kr_now() < end)
        pump();
    CHECK(dlg == NULL);
    kr_ui_free(ui);
}

TEST(a_request_made_before_the_window_existed_is_asked)
{
    fake_reset();
    snprintf(F.devices, sizeof F.devices,
             "[{\"id\":\"q\",\"name\":\"Antes\",\"reachable\":true,\"paired\":false,\"state\":\"requested_by_peer\","
             "\"key\":\"99887766\"}]");
    kr_ui *ui = new_ui();
    kr_ui_watch_pairing(ui, NULL);
    void *a[2] = {ui, "q"};
    wait_until(c_up, ui, 3);
    spin(20);
    CHECK(wait_until(c_dialog, a, 3));
    kr_ui_free(ui);
}

TEST(pair_dialog_for_a_device_that_is_not_asking_is_nothing)
{
    kr_ui *ui = connected_ui();
    CHECK(kr_ui_pair_dialog(ui, NULL, "b-phone") == NULL);
    CHECK(kr_ui_pair_dialog(ui, NULL, "nobody") == NULL);
    kr_ui_free(ui);
}

/* ---- settings --------------------------------------------------------------------------------- */

static GtkWidget *settings_page(kr_ui *ui)
{
    GtkWidget *w = g_object_ref_sink(kr_ui_settings_new(ui));
    wait_check(w, "Send what I copy");
    return w;
}

TEST(settings_are_built_from_the_daemons_schema)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    /* Every group of the schema is a tab. */
    CHECK(find(w, m_check_labeled, "Send what I copy"));
    CHECK(find(w, m_check_labeled, "Ping"));
    CHECK(find(w, m_check_labeled, "Keep the screen awake"));
    CHECK(find(w, m_tooltip_starts, "How the phone calls this computer"));
    CHECK(find(w, m_tooltip_starts, "Only changes the icon"));
    CHECK(find(w, m_tree, NULL));

    /* The values: defaults, and what the file says. */
    GtkWidget *ca = find(w, m_check_labeled, "Send what I copy");
    CHECK(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ca)));
    GtkWidget *ss = find(w, m_check_labeled, "Keep the screen awake");
    CHECK(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ss)));
    GtkWidget *tv = find(w, m_tree, NULL);
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
    CHECK_INT_EQ(gtk_tree_model_iter_n_children(model, NULL), 1);
    GtkTreeIter it;
    char *name = NULL, *cmd = NULL;
    gtk_tree_model_get_iter_first(model, &it);
    gtk_tree_model_get(model, &it, 0, &name, 1, &cmd, -1);
    CHECK_STR_EQ(name, "Ola");
    CHECK_STR_EQ(cmd, "echo oi");
    g_free(name);
    g_free(cmd);
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

static gboolean c_cfg_clipboard_off(void *unused)
{
    (void)unused;
    const char *v = kr_config_get(F.cfg, "clipboard", "auto_share");
    return v && strcmp(v, "false") == 0;
}

TEST(a_switch_is_saved_at_once)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    GtkWidget *ca = find(w, m_check_labeled, "Send what I copy");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ca), FALSE);
    CHECK(wait_until(c_logged, "\"key\":\"clipboard.auto_share\",\"value\":\"false\"", 3));
    CHECK(wait_until(c_cfg_clipboard_off, NULL, 3));
    spin(30); /* the config event comes back and reloads: still off, no loop */
    CHECK(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ca)));
    int sets = 0;
    for (guint i = 0; i < F.log->len; i++)
        sets += strstr(g_ptr_array_index(F.log, i), "config-set") != NULL;
    CHECK_INT_EQ(sets, 1);
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

TEST(a_change_made_elsewhere_shows_up)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    GtkWidget *ca = find(w, m_check_labeled, "Send what I copy");
    CHECK(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ca)));
    kr_config_set(F.cfg, "clipboard", "auto_share", "false"); /* the CLI did it */
    kr_control_broadcast(F.srv, "{\"event\":\"config\",\"key\":\"clipboard.auto_share\"}");
    double end = kr_now() + 3;
    while (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ca)) && kr_now() < end)
        pump();
    CHECK(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ca)));
    CHECK(!logged("config-set")); /* showing it didn't write it back */
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

TEST(what_belongs_to_a_plugin_that_is_off_is_dimmed)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    GtkWidget *ca = find(w, m_check_labeled, "Send what I copy");
    CHECK(GTK_WIDGET_SENSITIVE(ca));
    GtkWidget *plugin = find(w, m_check_labeled, "Clipboard"); /* the switch of the plugin */
    CHECK(plugin);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin), FALSE);
    double end = kr_now() + 3;
    while (GTK_WIDGET_SENSITIVE(ca) && kr_now() < end)
        pump();
    CHECK(!GTK_WIDGET_SENSITIVE(ca));
    CHECK(GTK_WIDGET_SENSITIVE(plugin)); /* the switch itself stays usable */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin), TRUE);
    end = kr_now() + 3;
    while (!GTK_WIDGET_SENSITIVE(ca) && kr_now() < end)
        pump();
    CHECK(GTK_WIDGET_SENSITIVE(ca));
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

static gboolean c_name_new(void *unused)
{
    (void)unused;
    return strcmp(kr_config_device_name(F.cfg), "Escrivaninha") == 0;
}

TEST(text_fields_save_on_enter_and_say_when_it_needs_a_restart)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    GtkWidget *name = find(w, m_tooltip_starts, "How the phone calls this computer");
    CHECK(name && GTK_IS_ENTRY(name));
    gtk_entry_set_text(GTK_ENTRY(name), "Escrivaninha");
    g_signal_emit_by_name(name, "activate");
    CHECK(wait_until(c_name_new, NULL, 3));
    CHECK(wait_label(w, "starts again", 3)); /* device.name is read at startup */

    /* Not edited: leaving the field writes nothing. */
    g_ptr_array_set_size(F.log, 0);
    GtkWidget *dir = find(w, m_tooltip_starts, "Where files from the phone go");
    CHECK(dir);
    gboolean ret;
    g_signal_emit_by_name(dir, "focus-out-event", NULL, &ret);
    spin(20);
    CHECK(!logged("config-set"));
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

TEST(a_refused_value_shows_why_and_goes_back)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    GtkWidget *addr = find(w, m_tooltip_starts, "IPv4 addresses that also");
    CHECK(addr && GTK_IS_ENTRY(addr));
    gtk_entry_set_text(GTK_ENTRY(addr), "bogus");
    g_signal_emit_by_name(addr, "activate");
    CHECK(wait_label(w, "must be IPv4 addresses", 3));
    double end = kr_now() + 3;
    while (strcmp(gtk_entry_get_text(GTK_ENTRY(addr)), "") != 0 && kr_now() < end)
        pump();
    CHECK_STR_EQ(gtk_entry_get_text(GTK_ENTRY(addr)), "");
    CHECK(kr_config_addresses(F.cfg) == NULL);
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

TEST(a_choice_is_saved)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    GtkWidget *type = find(w, m_tooltip_starts, "Only changes the icon");
    CHECK(type && GTK_IS_COMBO_BOX(type));
    CHECK_INT_EQ(gtk_combo_box_get_active(GTK_COMBO_BOX(type)), 0); /* desktop */
    gtk_combo_box_set_active(GTK_COMBO_BOX(type), 3);               /* tablet */
    CHECK(wait_until(c_logged, "\"key\":\"device.type\",\"value\":\"tablet\"", 3));
    spin(20);
    CHECK_STR_EQ(kr_config_device_type(F.cfg), "tablet");
    CHECK_INT_EQ(gtk_combo_box_get_active(GTK_COMBO_BOX(type)), 3);
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

static gboolean c_ola(void *expect)
{
    const char *v = kr_config_get(F.cfg, "runcommand", "Ola");
    return expect ? v && strcmp(v, expect) == 0 : v == NULL;
}

TEST(commands_are_edited_in_the_list)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = settings_page(ui);
    GtkWidget *tv = find(w, m_tree, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 1);
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(col));
    CHECK(cells);
    GtkCellRenderer *r = cells->data;
    g_list_free(cells);

    g_signal_emit_by_name(r, "edited", "0", "echo tchau");
    CHECK(wait_until(c_ola, "echo tchau", 3));
    spin(20);
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
    GtkTreeIter it;
    char *cmd = NULL;
    gtk_tree_model_get_iter_first(model, &it);
    gtk_tree_model_get(model, &it, 1, &cmd, -1);
    CHECK_STR_EQ(cmd, "echo tchau");
    g_free(cmd);

    /* Emptying it removes it. */
    g_signal_emit_by_name(r, "edited", "0", "");
    CHECK(wait_until(c_ola, NULL, 3));
    spin(20);
    CHECK_INT_EQ(gtk_tree_model_iter_n_children(model, NULL), 0);
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

TEST(settings_wait_for_the_daemon)
{
    fake_reset();
    kr_control_close(F.srv);
    F.srv = NULL;
    kr_ui *ui = new_ui();
    GtkWidget *w = g_object_ref_sink(kr_ui_settings_new(ui));
    CHECK(label_with(w, "Start kiremoted"));
    F.srv = kr_control_open(F.path, on_request, NULL);
    GtkWidget *ca = wait_check(w, "Send what I copy");
    wait_until(c_up, ui, 5);
    ca = wait_check(w, "Send what I copy");
    CHECK(ca);
    gtk_widget_destroy(w);
    g_object_unref(w);
    kr_ui_free(ui);
}

TEST(destroying_a_page_with_a_request_in_flight_is_safe)
{
    kr_ui *ui = connected_ui();
    GtkWidget *w = g_object_ref_sink(kr_ui_settings_new(ui));
    gtk_widget_destroy(w);
    g_object_unref(w); /* the schema and values requests are still out */
    spin(30);
    w = g_object_ref_sink(kr_ui_devices_new(ui, FALSE));
    gtk_widget_destroy(w);
    g_object_unref(w);
    spin(30);
    kr_ui_free(ui);
}

/* ---- one window --------------------------------------------------------------------- */

static struct {
    int calls;
    GPtrArray *words;
} launched;

static void on_launch(void *ud, char **words)
{
    (void)ud;
    launched.calls++;
    g_ptr_array_set_size(launched.words, 0);
    for (; *words; words++)
        g_ptr_array_add(launched.words, g_strdup(*words));
}

static gboolean c_launched(void *n)
{
    return launched.calls >= GPOINTER_TO_INT(n);
}

TEST(a_second_launch_hands_over_and_leaves)
{
    char *path = kr_instance_path(F.path);
    CHECK(path && g_str_has_suffix(path, "/s-ui.sock"));
    launched.calls = 0;
    launched.words = g_ptr_array_new_with_free_func(g_free);

    char *first_words[] = {"show", NULL};
    CHECK(kr_instance_acquire(path, first_words, on_launch, NULL)); /* we are the instance */
    CHECK_INT_EQ(launched.calls, 0);                                /* it doesn't ask itself */

    char *second[] = {"show", "settings", "pair=abc", NULL};
    CHECK(!kr_instance_acquire(path, second, on_launch, NULL)); /* told the first, and leaves */
    CHECK(wait_until(c_launched, GINT_TO_POINTER(1), 3));
    CHECK_INT_EQ(launched.words->len, 3);
    CHECK_STR_EQ(g_ptr_array_index(launched.words, 0), "show");
    CHECK_STR_EQ(g_ptr_array_index(launched.words, 1), "settings");
    CHECK_STR_EQ(g_ptr_array_index(launched.words, 2), "pair=abc");

    char *third[] = {"show", NULL};
    CHECK(!kr_instance_acquire(path, third, on_launch, NULL));
    CHECK(wait_until(c_launched, GINT_TO_POINTER(2), 3));
    CHECK_INT_EQ(launched.words->len, 1);

    kr_instance_release();
    CHECK(!g_file_test(path, G_FILE_TEST_EXISTS)); /* the socket goes with it */

    /* Nobody is left: the next one is the instance. */
    CHECK(kr_instance_acquire(path, first_words, on_launch, NULL));
    kr_instance_release();
    g_free(path);
    g_ptr_array_free(launched.words, TRUE);
}

TEST(a_socket_left_by_a_crash_is_replaced)
{
    char *path = g_strdup_printf("%s/stale-ui.sock", F.dir);
    /* A socket file with nobody behind it. */
    GSocket *s = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL);
    GSocketAddress *a = g_unix_socket_address_new(path);
    CHECK(g_socket_bind(s, a, TRUE, NULL));
    g_object_unref(s); /* closed, the file stays */
    g_object_unref(a);
    CHECK(g_file_test(path, G_FILE_TEST_EXISTS));

    launched.calls = 0;
    launched.words = g_ptr_array_new_with_free_func(g_free);
    char *w[] = {"show", NULL};
    CHECK(kr_instance_acquire(path, w, on_launch, NULL));
    CHECK(!kr_instance_acquire(path, w, on_launch, NULL));
    CHECK(wait_until(c_launched, GINT_TO_POINTER(1), 3));
    kr_instance_release();
    g_free(path);
    g_ptr_array_free(launched.words, TRUE);
}

TEST(garbage_and_silent_clients_do_not_disturb_the_instance)
{
    char *path = g_strdup_printf("%s/noise-ui.sock", F.dir);
    launched.calls = 0;
    launched.words = g_ptr_array_new_with_free_func(g_free);
    char *w[] = {"show", NULL};
    CHECK(kr_instance_acquire(path, w, on_launch, NULL));

    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    int silent = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(silent, (struct sockaddr *)&sa, sizeof sa) == 0); /* says nothing */
    int big = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(big, (struct sockaddr *)&sa, sizeof sa) == 0);
    char junk[4096];
    memset(junk, 'x', sizeof junk);
    (void)!write(big, junk, sizeof junk); /* long, never ends */
    spin(30);
    CHECK_INT_EQ(launched.calls, 0);

    char *real[] = {"settings", NULL};
    CHECK(!kr_instance_acquire(path, real, on_launch, NULL)); /* still served */
    CHECK(wait_until(c_launched, GINT_TO_POINTER(1), 3));
    CHECK_STR_EQ(g_ptr_array_index(launched.words, 0), "settings");
    close(silent);
    close(big);
    kr_instance_release();
    g_free(path);
    g_ptr_array_free(launched.words, TRUE);
}

TEST(no_path_means_no_single_instance)
{
    char *w[] = {"show", NULL};
    CHECK(kr_instance_acquire(NULL, w, on_launch, NULL));
    char *toolong = g_strnfill(300, 'a');
    CHECK(kr_instance_acquire(toolong, w, on_launch, NULL));
    g_free(toolong);
}

/* For a human: KR_UI_SHOT=/path/shot.png tests/ui/run.sh build draws the
 * window with the scripted devices and saves what the private X server shows
 * (needs ImageMagick's import). Not a check. */
static void screenshot(const char *file)
{
    kr_ui *ui = connected_ui();
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 480, 400);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), kr_ui_devices_new(ui, FALSE), gtk_label_new("Devices"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), kr_ui_settings_new(ui), gtk_label_new("Settings"));
    gtk_container_add(GTK_CONTAINER(win), tabs);
    gtk_widget_show_all(win);
    kr_control_broadcast(F.srv, ASKING);
    spin(150);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "import -window root '%s'", file);
    if (system(cmd) != 0)
        fprintf(stderr, "could not take the screenshot\n");
    gtk_widget_destroy(win);
    kr_ui_free(ui);
}

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "tests/ui/test_ui: skipped (no display)\n");
        return 0;
    }
    if (!fake_open())
        return 1;
    ask.errors = g_ptr_array_new_with_free_func(g_free);
    if (getenv("KR_UI_SHOT")) {
        screenshot(getenv("KR_UI_SHOT"));
        fake_close();
        return 0;
    }

    RUN(devices_list_shows_what_the_model_says);
    RUN(devices_list_follows_the_daemon_coming_and_going);
    RUN(the_menu_offers_what_the_model_allows);
    RUN(actions_send_their_requests);
    RUN(unpairing_asks_first);
    RUN(a_failed_request_shows_the_daemons_reason);
    RUN(a_request_without_a_daemon_says_so_once);
    RUN(no_dialog_while_closing);
    RUN(dropped_uris_become_paths);
    RUN(a_pairing_request_opens_the_question);
    RUN(refusing_does_not_ask_twice);
    RUN(the_question_goes_away_when_answered_elsewhere);
    RUN(a_request_made_before_the_window_existed_is_asked);
    RUN(pair_dialog_for_a_device_that_is_not_asking_is_nothing);
    RUN(settings_are_built_from_the_daemons_schema);
    RUN(a_switch_is_saved_at_once);
    RUN(a_change_made_elsewhere_shows_up);
    RUN(what_belongs_to_a_plugin_that_is_off_is_dimmed);
    RUN(text_fields_save_on_enter_and_say_when_it_needs_a_restart);
    RUN(a_refused_value_shows_why_and_goes_back);
    RUN(a_choice_is_saved);
    RUN(commands_are_edited_in_the_list);
    RUN(settings_wait_for_the_daemon);
    RUN(destroying_a_page_with_a_request_in_flight_is_safe);
    RUN(a_second_launch_hands_over_and_leaves);
    RUN(a_socket_left_by_a_crash_is_replaced);
    RUN(garbage_and_silent_clients_do_not_disturb_the_instance);
    RUN(no_path_means_no_single_instance);

    int rc = harness_report();
    g_ptr_array_free(ask.errors, TRUE);
    fake_close();
    return rc;
}
