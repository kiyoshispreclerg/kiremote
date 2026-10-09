/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "i18n.h"

static const char *prog = "kiremote";
static bool verbose;

void kr_log_init(const char *program)
{
    prog = program;
}

void kr_log_set_verbose(bool on)
{
    verbose = on;
}

bool kr_log_verbose(void)
{
    return verbose;
}

__attribute__((format(printf, 2, 0)))
static void vlog(const char *level, const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: %s", prog, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void kr_log_debug(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vlog("debug: ", fmt, ap);
    va_end(ap);
}

void kr_log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void kr_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    /* TRANSLATORS: log prefix, keep the trailing ": ". */
    vlog(_("warning: "), fmt, ap);
    va_end(ap);
}

void kr_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    /* TRANSLATORS: log prefix, keep the trailing ": ". */
    vlog(_("error: "), fmt, ap);
    va_end(ap);
}
