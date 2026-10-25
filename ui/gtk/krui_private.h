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

/* kiremote-mirror's absolute path: $KIREMOTE_MIRROR, next to this
 * executable (a build tree), else from the PATH. NULL if there is none. */
const char *kr_ui_mirror_program(void);

/* What is missing for mirroring and the camera (kiremote-mirror, adb), as a
 * translated sentence for the settings pages; NULL when both are there. adb
 * is looked for each time, so installing it shows without a restart of the
 * settings window (the actions follow at the next start). */
const char *kr_ui_mirror_problem(void);

/* Sends req (taken over) and shows the error, if any, over parent. */
void kr_ui_request(kr_ui *ui, GtkWindow *parent, cJSON *req);

#endif
