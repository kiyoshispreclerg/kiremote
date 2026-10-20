/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_UI_PRIVATE_H
#define KR_UI_PRIVATE_H

/* What the files of libkrui share and the hosts don't see. */

#include "krui.h"

const kr_ui_hooks *kr_ui_get_hooks(kr_ui *ui);
/* The open pairing dialogs (GtkWidget *). */
GList **kr_ui_pair_dialogs(kr_ui *ui);

/* mem (from g_malloc) is freed when the kr_ui is: for what a listener keeps. */
void kr_ui_own(kr_ui *ui, gpointer mem);

/* The colors set by kr_ui_set_colors; FALSE if none. */
gboolean kr_ui_colors(kr_ui *ui, GdkColor *fg, GdkColor *bg);

/* Is kr_ui_free running? */
gboolean kr_ui_closing(kr_ui *ui);

/* Sends req (taken over) and shows the error, if any, over parent. */
void kr_ui_request(kr_ui *ui, GtkWindow *parent, cJSON *req);

#endif
