/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_UTIL_H
#define KR_UTIL_H

#include <stdbool.h>
#include <stddef.h>

#define KR_PATH_MAX 4096

/* Reads a whole file into a NUL-terminated buffer; NULL on error (errno set).
 * The caller frees it. */
char *kr_read_file(const char *path, size_t *len);

bool kr_file_exists(const char *path);
bool kr_dir_exists(const char *path);

/* mkdir -p for the directory part of path (errors are left to whoever
 * then creates the file). */
void kr_make_parent_dirs(const char *path);

/* snprintf for paths: false (and an empty string) if it would not fit. */
bool kr_pathf(char *out, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Monotonic clock in seconds. */
double kr_now(void);

/* $XDG_CONFIG_HOME/kiremote/<rel> (default ~/.config/kiremote/<rel>). */
bool kr_config_path(char *out, size_t size, const char *rel);
/* $XDG_RUNTIME_DIR/<name>; without it, /tmp/<name>-<uid>. */
bool kr_runtime_path(char *out, size_t size, const char *name);

#endif
