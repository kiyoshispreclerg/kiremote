/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_I18N_H
#define KR_I18N_H

/* gettext with English msgids; translations live in po/. Every program
 * calls kr_i18n_init() first thing in main(). */

#include <libintl.h>

#define _(s) gettext(s)
#define N_(s) (s)

/* setlocale + bindtextdomain. The catalog is looked up next to the
 * executable first (build/locale, so a fresh build runs translated without
 * installing), then in LOCALEDIR. */
void kr_i18n_init(void);

#endif
