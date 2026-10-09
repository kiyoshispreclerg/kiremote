/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char *kr_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    char *buf = NULL;
    size_t size = 0, cap = 0;
    for (;;) {
        if (cap - size < 65536) {
            cap = cap ? cap * 2 : 1 << 20;
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fclose(f);
                errno = ENOMEM;
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + size, 1, cap - size, f);
        size += n;
        if (n == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        errno = EIO;
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    if (len)
        *len = size;
    return buf;
}

bool kr_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool kr_dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool kr_pathf(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size) {
        if (size)
            out[0] = '\0';
        return false;
    }
    return true;
}

void kr_make_parent_dirs(const char *path)
{
    char buf[KR_PATH_MAX];
    if (!kr_pathf(buf, sizeof buf, "%s", path))
        return;
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
}

double kr_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* $VAR/kiremote/<rel>, or $HOME/<fallback>/kiremote/<rel>. */
static bool xdg_path(char *out, size_t size, const char *var,
                     const char *fallback, const char *rel)
{
    const char *base = getenv(var);
    if (base && base[0] == '/')
        return kr_pathf(out, size, "%s/kiremote/%s", base, rel);
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') {
        if (size)
            out[0] = '\0';
        return false;
    }
    return kr_pathf(out, size, "%s/%s/kiremote/%s", home, fallback, rel);
}

bool kr_config_path(char *out, size_t size, const char *rel)
{
    return xdg_path(out, size, "XDG_CONFIG_HOME", ".config", rel);
}

bool kr_runtime_path(char *out, size_t size, const char *name)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && dir[0] == '/')
        return kr_pathf(out, size, "%s/%s", dir, name);
    return kr_pathf(out, size, "/tmp/%s-%lu", name, (unsigned long)getuid());
}
