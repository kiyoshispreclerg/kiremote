/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_I18N_H
#define KR_I18N_H

/* gettext with English msgids; translations live in po/. Every program
 * calls kr_i18n_init() first thing in main(). */

#include <libintl.h>

/* The domain is named, not the process's default: the KiDesktop modules run
 * inside kiconf, xisserve and xispanel, which have their own. */
#define KR_DOMAIN "kiremote"
#define _(s) dgettext(KR_DOMAIN, (s))
#define N_(s) (s)

/* setlocale + bindtextdomain. The catalog is looked up next to the
 * executable first (build/locale, so a fresh build runs translated without
 * installing), then in LOCALEDIR. */
void kr_i18n_init(void);

/* For code that runs inside another program (the KiDesktop modules): only
 * binds the domain to its catalogs (the host did setlocale). $KIREMOTE_LOCALEDIR
 * overrides the directory, for a build tree. Safe to call again. */
void kr_i18n_bind(void);

#endif
