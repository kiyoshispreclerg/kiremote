/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_UI_H
#define KR_UI_H

/* libkrui: the whole Kiremote interface as GTK2 widgets, with no window of
 * its own. The standalone program (ui/gtk/main.c) and the KiDesktop modules
 * (kiconf tab, xisserve page) are hosts that put these widgets somewhere,
 * so the two interfaces cannot differ in what they do or offer.
 *
 * A kr_ui is the connection to kiremoted (a kr_client driven by GLib's main
 * loop), what the widgets share. Widgets must be destroyed before the
 * kr_ui is freed. Everything runs in the GTK main loop; nothing blocks. */

#include <gtk/gtk.h>

#include "krclient.h"
#include "krmodel.h"

typedef struct kr_ui kr_ui;

/* socket_path NULL: the default one. The connection is tried at once and
 * retried by itself. */
kr_ui *kr_ui_new(const char *socket_path);
void kr_ui_free(kr_ui *ui);
kr_client *kr_ui_client(kr_ui *ui);
const kr_model *kr_ui_model(kr_ui *ui);

/* Told of every change the daemon reports (after the model is updated).
 * Widgets listen while they live. */
typedef void (*kr_ui_listener)(void *ud, kr_change what, const char *id);
guint kr_ui_listen(kr_ui *ui, kr_ui_listener fn, void *ud);
void kr_ui_unlisten(kr_ui *ui, guint id);

/* For a host whose background is not the GTK theme's (the xisserve popup
 * draws its own): the text and background colors the lists use. NULL for
 * both: the theme's. Set before creating the widgets. */
void kr_ui_set_colors(kr_ui *ui, const GdkColor *fg, const GdkColor *bg);

/* ---- the asking, replaceable (the tests answer for the user) ---- */

typedef struct {
    /* "Unpair from X?" and the like. */
    gboolean (*confirm)(GtkWindow *parent, const char *question);
    /* Files to send, absolute paths, NULL-terminated, g_strfreev; NULL if canceled. */
    char **(*choose_files)(GtkWindow *parent);
    /* Text to send; g_free; NULL if canceled. */
    char *(*ask_text)(GtkWindow *parent);
    /* A request failed. */
    void (*show_error)(GtkWindow *parent, const char *message);
    /* Runs a program of ours (kiremote-mirror) on its own: argv[0] is an
     * absolute path. FALSE if it could not start (the error is shown). */
    gboolean (*spawn)(GtkWindow *parent, const char *const *argv);
} kr_ui_hooks;
void kr_ui_set_hooks(kr_ui *ui, const kr_ui_hooks *hooks);
/* The ones a user answers (GTK dialogs), for a host that wraps them. */
void kr_ui_default_hooks(kr_ui_hooks *out);

/* ---- widgets ---- */

/* The devices: a row each with icon, name, state, battery, signal and a menu
 * of actions; pairing buttons; "look for devices". compact: less padding,
 * for a popup. Accepts files dropped on a connected device. */
GtkWidget *kr_ui_devices_new(kr_ui *ui, gboolean compact);
/* Puts a button on the left of the row at the foot of the list, the one
 * "look for devices" is on the right of. */
void kr_ui_devices_add_footer(GtkWidget *devices, GtkWidget *button);

/* All the settings, built from what the daemon says it has (config-schema),
 * one tab per group; saves each change as it is made. */
GtkWidget *kr_ui_settings_new(kr_ui *ui);

/* The menu of a device's actions, as kr_dev_actions says. Parent: the
 * window for the dialogs the actions open. */
GtkWidget *kr_ui_device_menu_new(kr_ui *ui, GtkWindow *parent, const char *device_id);

/* Does what the user picked: asks for more if needed (files, text), confirms
 * what undoes something, sends the request, shows the error if it fails. */
void kr_ui_run_action(kr_ui *ui, GtkWindow *parent, kr_action action, const char *device_id);

/* Sends files (absolute paths). */
void kr_ui_send_files(kr_ui *ui, GtkWindow *parent, const char *device_id, const char *const *paths);
/* "file:///a%20b" ... → {"/a b", ...}, g_strfreev; the ones that aren't local files are dropped. */
char **kr_ui_paths_from_uris(char **uris);

/* ---- pairing ---- */

/* Shows a dialog whenever a device asks to pair (with its verification key,
 * to compare with the phone's), and closes it if the request ends. */
void kr_ui_watch_pairing(kr_ui *ui, GtkWindow *parent);
/* The dialog for that device now, or NULL. Shows one if the device is
 * asking and has none (for "kiremote --pair ID"). */
GtkWidget *kr_ui_pair_dialog(kr_ui *ui, GtkWindow *parent, const char *device_id);

#endif
