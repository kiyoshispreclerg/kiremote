/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiremote: the window (devices and settings) for any desktop, plus a tray
 * icon where the desktop has no panel widget of its own. All the interface is
 * libkrui's; this only puts it in a window. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "krui.h"
#include "log.h"

typedef struct {
    kr_ui *ui;
    GtkWidget *window, *tabs;
    GtkStatusIcon *icon;
    gboolean tray_wanted;
    char tray_setting[16]; /* "auto", "yes" or "no", as the daemon says */
    const char *pair_id;
} app;

static void usage(FILE *out)
{
    fprintf(out, _("Usage: %s [OPTION]...\n"
                   "The Kiremote window: devices, pairing and settings.\n"
                   "\n"
                   "  -s, --socket PATH   the daemon's control socket\n"
                   "      --settings      open on the settings page\n"
                   "      --pair ID       show the pairing question of that device\n"
                   "      --hidden        start in the tray, without the window\n"
                   "  -V, --version       show the version and exit\n"
                   "  -h, --help          show this help and exit\n"),
            "kiremote");
}

/* ---- the tray ------------------------------------------------------------------- */

static gboolean desktop_has_own_icon(void)
{
    const char *d = g_getenv("XDG_CURRENT_DESKTOP");
    return d && g_ascii_strcasecmp(d, "KiDesktop") == 0;
}

static void toggle_window(app *a)
{
    if (gtk_widget_get_visible(a->window) && gtk_window_is_active(GTK_WINDOW(a->window))) {
        gtk_widget_hide(a->window);
    } else {
        gtk_window_present(GTK_WINDOW(a->window));
    }
}

static void icon_activate(GtkStatusIcon *i, gpointer data)
{
    (void)i;
    toggle_window(data);
}

static void menu_show(GtkMenuItem *m, gpointer data)
{
    (void)m;
    gtk_window_present(GTK_WINDOW(((app *)data)->window));
}

static void menu_quit(GtkMenuItem *m, gpointer data)
{
    (void)m;
    (void)data;
    gtk_main_quit();
}

static void icon_popup(GtkStatusIcon *i, guint button, guint time, gpointer data)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *show = gtk_menu_item_new_with_label(_("Show Kiremote"));
    GtkWidget *quit = gtk_menu_item_new_with_label(_("Quit"));
    g_signal_connect(show, "activate", G_CALLBACK(menu_show), data);
    g_signal_connect(quit, "activate", G_CALLBACK(menu_quit), data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, i, button, time);
}

static void update_icon(app *a)
{
    if (!a->icon)
        return;
    char tip[400];
    kr_model_tooltip(kr_ui_model(a->ui), tip, sizeof tip);
    gtk_status_icon_set_tooltip_text(a->icon, tip);
    gtk_status_icon_set_blinking(a->icon, kr_model_pair_pending(kr_ui_model(a->ui)));
}

static void apply_tray(app *a)
{
    gboolean want = strcmp(a->tray_setting, "yes") == 0 ||
                    (strcmp(a->tray_setting, "no") != 0 && !desktop_has_own_icon());
    if (want && !a->icon) {
        a->icon = gtk_status_icon_new_from_icon_name("phone");
        g_signal_connect(a->icon, "activate", G_CALLBACK(icon_activate), a);
        g_signal_connect(a->icon, "popup-menu", G_CALLBACK(icon_popup), a);
        update_icon(a);
    } else if (!want && a->icon) {
        g_object_unref(a->icon);
        a->icon = NULL;
        gtk_widget_show(a->window); /* nothing else would bring it back */
    }
    a->tray_wanted = want;
}

static void on_tray_reply(void *ud, const cJSON *reply)
{
    app *a = ud;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply, "values"), "ui.tray");
    if (reply && !kr_reply_error(reply) && cJSON_IsString(v)) {
        snprintf(a->tray_setting, sizeof a->tray_setting, "%s", v->valuestring);
        apply_tray(a);
    }
}

static void ask_tray_setting(app *a)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "config-get");
    kr_client_send(kr_ui_client(a->ui), r, on_tray_reply, a);
}

static void on_change(void *ud, kr_change what, const char *id)
{
    app *a = ud;
    if (what == KR_CH_DAEMON && kr_model_daemon_up(kr_ui_model(a->ui))) {
        ask_tray_setting(a);
        if (a->pair_id) {
            kr_ui_pair_dialog(a->ui, GTK_WINDOW(a->window), a->pair_id);
            a->pair_id = NULL;
        }
    } else if (what == KR_CH_CONFIG && id && strcmp(id, "ui.tray") == 0) {
        ask_tray_setting(a);
    } else if (what == KR_CH_PAIR_REQUEST && !gtk_widget_get_visible(a->window) && !a->icon) {
        gtk_window_present(GTK_WINDOW(a->window)); /* no tray to blink: show the question's window */
    }
    update_icon(a);
}

static gboolean window_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    app *a = data;
    (void)e;
    if (a->icon) {
        gtk_widget_hide(w);
        return TRUE;
    }
    gtk_main_quit();
    return FALSE;
}

int main(int argc, char **argv)
{
    kr_i18n_init();
    kr_log_init("kiremote");
    /* --help and --version must not need a display. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("kiremote %s\n", KR_VERSION);
            return 0;
        }
    }
    /* GTK would call setlocale(LC_ALL) and undo kr_i18n_init's LC_NUMERIC=C. */
    gtk_disable_setlocale();
    if (!gtk_init_check(&argc, &argv)) {
        kr_log_error(_("cannot open the display"));
        return 1;
    }

    enum { O_SETTINGS = 256, O_PAIR, O_HIDDEN };
    static const struct option longopts[] = {
        {"socket", required_argument, NULL, 's'},     {"settings", no_argument, NULL, O_SETTINGS},
        {"pair", required_argument, NULL, O_PAIR},    {"hidden", no_argument, NULL, O_HIDDEN},
        {"version", no_argument, NULL, 'V'},          {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *socket_path = NULL;
    app a = {0};
    gboolean settings = FALSE, hidden = FALSE;
    int c;
    while ((c = getopt_long(argc, argv, "s:Vh", longopts, NULL)) != -1) {
        switch (c) {
        case 's':
            socket_path = optarg;
            break;
        case O_SETTINGS:
            settings = TRUE;
            break;
        case O_PAIR:
            a.pair_id = optarg;
            break;
        case O_HIDDEN:
            hidden = TRUE;
            break;
        case 'V':
            printf("kiremote %s\n", KR_VERSION);
            return 0;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind < argc) {
        usage(stderr);
        return 2;
    }

    a.ui = kr_ui_new(socket_path);
    if (!a.ui) {
        kr_log_error(_("out of memory"));
        return 1;
    }
    snprintf(a.tray_setting, sizeof a.tray_setting, "auto");

    a.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a.window), "Kiremote");
    gtk_window_set_icon_name(GTK_WINDOW(a.window), "phone");
    gtk_window_set_default_size(GTK_WINDOW(a.window), 480, 560);
    g_signal_connect(a.window, "delete-event", G_CALLBACK(window_delete), &a);

    a.tabs = gtk_notebook_new();
    GtkWidget *devices = kr_ui_devices_new(a.ui, FALSE);
    GtkWidget *config = kr_ui_settings_new(a.ui);
    gtk_container_set_border_width(GTK_CONTAINER(devices), 8);
    gtk_container_set_border_width(GTK_CONTAINER(config), 8);
    gtk_notebook_append_page(GTK_NOTEBOOK(a.tabs), devices, gtk_label_new(_("Devices")));
    gtk_notebook_append_page(GTK_NOTEBOOK(a.tabs), config, gtk_label_new(_("Settings")));
    gtk_container_add(GTK_CONTAINER(a.window), a.tabs);
    gtk_widget_show_all(a.tabs);
    if (settings)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(a.tabs), 1);

    kr_ui_watch_pairing(a.ui, GTK_WINDOW(a.window));
    kr_ui_listen(a.ui, on_change, &a);
    apply_tray(&a);
    if (!hidden || !a.icon || a.pair_id)
        gtk_widget_show(a.window);

    gtk_main();

    if (a.icon)
        g_object_unref(a.icon);
    gtk_widget_destroy(a.window); /* the widgets before the connection they listen to */
    kr_ui_free(a.ui);
    return 0;
}
