/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"

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

bool kr_text_usable(const char *text, size_t len)
{
    return len > 0 && strlen(text) == len;
}

bool kr_pem_text_ok(const char *text, size_t len)
{
    return kr_text_usable(text, len) && strstr(text, "-----BEGIN ") && strstr(text, "-----END ");
}

int kr_flush_sync(FILE *f)
{
    if (fflush(f) != 0 || ferror(f))
        return -1;
    return fsync(fileno(f));
}

char *kr_read_file_or_backup(const char *path, kr_text_check ok, size_t *len)
{
    size_t n = 0;
    char *text = kr_read_file(path, &n);
    int err = errno;
    if (text && (!ok || ok(text, n))) {
        if (len)
            *len = n;
        return text;
    }
    char bak[KR_PATH_MAX];
    size_t bn = 0;
    char *copy = NULL;
    if (kr_pathf(bak, sizeof bak, "%s.backup", path) && (copy = kr_read_file(bak, &bn)) &&
        (!ok || ok(copy, bn))) {
        if (text)
            kr_log_warn(_("%s is corrupt, using the copy %s"), path, bak);
        else
            kr_log_warn(_("%s is missing, using the copy %s"), path, bak);
        free(text);
        if (len)
            *len = bn;
        return copy;
    }
    free(copy);
    if (len)
        *len = n;
    errno = err;
    return text;
}

int kr_backup_file(const char *path, kr_text_check check)
{
    char bak[KR_PATH_MAX], tmp[KR_PATH_MAX];
    if (!kr_pathf(bak, sizeof bak, "%s.backup", path) ||
        !kr_pathf(tmp, sizeof tmp, "%s.backup.tmp", path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int in = open(path, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(in, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(in);
        return 0;
    }
    if (check) {
        size_t n;
        char *text = kr_read_file(path, &n);
        bool good = text && check(text, n);
        free(text);
        if (!good) {
            close(in);
            kr_log_warn(_("%s looks corrupt; the earlier copy %s was kept"), path, bak);
            return 0;
        }
    }
    /* Same mode as the original: it may hold the private key. */
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777);
    if (out < 0) {
        int e = errno;
        close(in);
        errno = e;
        return -1;
    }
    bool ok = fchmod(out, st.st_mode & 0777) == 0;
    char buf[8192];
    while (ok) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        for (ssize_t off = 0; ok && off < n;) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0)
                ok = false;
            else
                off += w;
        }
    }
    ok = ok && fsync(out) == 0;
    int e = errno;
    close(in);
    ok = close(out) == 0 && ok;
    if (!ok || rename(tmp, bak) < 0) {
        if (ok)
            e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
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

bool kr_cache_path(char *out, size_t size, const char *rel)
{
    return xdg_path(out, size, "XDG_CACHE_HOME", ".cache", rel);
}

bool kr_runtime_path(char *out, size_t size, const char *name)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && dir[0] == '/')
        return kr_pathf(out, size, "%s/%s", dir, name);
    return kr_pathf(out, size, "/tmp/%s-%lu", name, (unsigned long)getuid());
}

bool kr_utf8_valid(const char *str, size_t len)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    while (i < len) {
        unsigned c = s[i];
        size_t n;
        unsigned cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            n = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + n > len)
            return false;
        for (size_t k = 1; k < n; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n;
    }
    return true;
}

char *kr_latin1_to_utf8(const char *str, size_t len)
{
    const unsigned char *s = (const unsigned char *)str;
    char *out = malloc(len * 2 + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < 0x80) {
            out[o++] = (char)s[i];
        } else {
            out[o++] = (char)(0xC0 | (s[i] >> 6));
            out[o++] = (char)(0x80 | (s[i] & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}
