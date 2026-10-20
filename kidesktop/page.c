/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The xisserve page (`xisserve --kiremote`): what the KDE Connect tray popup
 * shows, in the panel's popup: the devices with battery and signal, an
 * actions menu on each, pair/unpair, look for devices, and a way to the
 * settings. The widget is libkrui's; this only puts it in a page.
 *
 * The page is a popup that closes when it loses the grab, so a question may
 * not open a window of its own: confirmations go to `xisserve --question`,
 * and the file and text pickers tell xisserve a window of ours is about to
 * take the grab (xisserve_transient_popup_begin). */
#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

#include "i18n.h"
#include "krui.h"
#include "xisserve_page.h"

static kr_ui *g_ui;
static kr_ui_hooks g_default; /* the dialogs libkrui has */

/* ---- the answers, without leaving the popup ----------------------------------- */

typedef struct {
    GMainLoop *loop;
    GPid pid;
    int status;
    gboolean done;
} child;

static void child_done(GPid pid, gint status, gpointer data)
{
    child *c = data;
    c->status = status;
    c->done = TRUE;
    g_spawn_close_pid(pid);
    if (c->loop)
        g_main_loop_quit(c->loop);
}

/* Runs `xisserve --question` and waits for it with the loop running (a
 * blocked loop would freeze the popup under it). TRUE if it answered 1. */
static gboolean ask_yes_no(const char *text)
{
    char *t = g_strdup_printf("--text=%s", text);
    char *no = g_strdup_printf("--button=%s:0", _("No"));
    char *yes = g_strdup_printf("--button=%s:1", _("Yes"));
    char *argv[] = {"xisserve", "--question", t, no, yes, NULL};
    GPid pid;
    gboolean yes_answer = FALSE;
    gint out = -1;
    if (g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL,
                                 &out, NULL, NULL)) {
        child c = {.loop = g_main_loop_new(NULL, FALSE), .pid = pid};
        g_child_watch_add(pid, child_done, &c);
        if (!c.done)
            g_main_loop_run(c.loop);
        g_main_loop_unref(c.loop);
        c.loop = NULL;
        char buf[16] = "";
        ssize_t n = read(out, buf, sizeof buf - 1);
        yes_answer = n > 0 && buf[0] == '1';
        close(out);
    }
    g_free(t);
    g_free(no);
    g_free(yes);
    return yes_answer;
}

static gboolean page_confirm(GtkWindow *parent, const char *question)
{
    (void)parent;
    return ask_yes_no(question);
}

static char **page_choose_files(GtkWindow *parent)
{
    xisserve_transient_popup_begin();
    char **r = g_default.choose_files(parent);
    xisserve_transient_popup_end();
    return r;
}

static char *page_ask_text(GtkWindow *parent)
{
    xisserve_transient_popup_begin();
    char *r = g_default.ask_text(parent);
    xisserve_transient_popup_end();
    return r;
}

static void page_show_error(GtkWindow *parent, const char *message)
{
    xisserve_transient_popup_begin();
    g_default.show_error(parent, message);
    xisserve_transient_popup_end();
}

/* ---- the page --------------------------------------------------------------------- */

static void open_settings(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    run_detached("kiconf --tab kiremote");
    xisserve_hide();
}

static GtkWidget *build(void)
{
    kr_i18n_bind();
    g_ui = kr_ui_new(NULL);
    if (!g_ui)
        return gtk_label_new(_("Out of memory"));
    /* The popup has its own colors (xisserve --fg/--bg), not the GTK theme's. */
    double r, g, b, a;
    GdkColor fg, bg;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    fg = (GdkColor){0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    xisserve_get_bg_rgba(&r, &g, &b, &a);
    bg = (GdkColor){0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    kr_ui_set_colors(g_ui, &fg, &bg);
    kr_ui_default_hooks(&g_default);
    kr_ui_hooks hooks = {page_confirm, page_choose_files, page_ask_text, page_show_error};
    kr_ui_set_hooks(g_ui, &hooks);

    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(box), kr_ui_devices_new(g_ui, TRUE), TRUE, TRUE, 0);

    GtkWidget *settings = gtk_button_new_with_label(_("Settings..."));
    g_signal_connect(settings, "clicked", G_CALLBACK(open_settings), NULL);
    GtkWidget *bar = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), settings, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    return box;
}

static const XisservePageModule mod = {
    .abi = XISSERVE_PAGE_ABI,
    .title = N_("Kiremote"),
    .gettext_domain = KR_DOMAIN,
    .min_width = 380,
    .min_height = 420,
    .build = build,
};

const XisservePageModule *xisserve_page_entry(void); /* looked up by xisserve */

const XisservePageModule *xisserve_page_entry(void)
{
    return &mod;
}
