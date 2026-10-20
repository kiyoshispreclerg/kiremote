/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The kiconf tab: the Kiremote settings, the same screen the standalone
 * window has (libkrui's kr_ui_settings_new). kiconf loads this module on the
 * first visit to the tab (kiremote.desktop, X-KiConf-Module). */
#include <gtk/gtk.h>

#include "i18n.h"
#include "krui.h"

static kr_ui *g_ui; /* the module is never unloaded: lives as long as kiconf */

GtkWidget *kiconf_tab_build(void); /* the symbol kiconf looks up */

GtkWidget *kiconf_tab_build(void)
{
    kr_i18n_bind();
    if (!g_ui)
        g_ui = kr_ui_new(NULL);
    if (!g_ui)
        return gtk_label_new(_("Out of memory"));
    GtkWidget *w = kr_ui_settings_new(g_ui);
    gtk_container_set_border_width(GTK_CONTAINER(w), 12);
    return w;
}
