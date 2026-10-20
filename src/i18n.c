/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "i18n.h"

#include <locale.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/local/share/locale"
#endif

void kr_i18n_init(void)
{
    setlocale(LC_ALL, "");
    /* The wire format is JSON: strtod()/printf() must keep "." as the decimal
     * point, or every fractional number from the phone (the presenter's
     * dx/dy) fails to parse under pt_BR and the like. */
    setlocale(LC_NUMERIC, "C");

    const char *dir = LOCALEDIR;
    char exe[KR_PATH_MAX], local[KR_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            if (kr_pathf(local, sizeof local, "%s/locale", exe) && kr_dir_exists(local))
                dir = local;
        }
    }
    bindtextdomain("kiremote", dir);
    bind_textdomain_codeset("kiremote", "UTF-8");
    textdomain("kiremote");
}
