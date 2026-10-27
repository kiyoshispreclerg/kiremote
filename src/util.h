/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_UTIL_H
#define KR_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define KR_PATH_MAX 4096

/* Reads a whole file into a NUL-terminated buffer; NULL on error (errno set).
 * The caller frees it. */
char *kr_read_file(const char *path, size_t *len);

/* Judges a file's contents (NUL-terminated, len bytes). */
typedef bool (*kr_text_check)(const char *text, size_t len);

/* Copies path to "<path>.backup" (same folder, same mode, fsync'd, replaced
 * atomically) so a crash while the file is rewritten leaves a good copy.
 * Call it before writing the new contents. Only a file that passes ok (NULL:
 * anything) is copied, so a corrupt file never replaces a good backup (that
 * is logged, and is not an error). A missing file is not an error either
 * (0, nothing to back up); -1 with errno otherwise. */
int kr_backup_file(const char *path, kr_text_check ok);

/* kr_read_file, but when path is missing or fails ok and "<path>.backup"
 * passes, the backup's contents (logged). With no usable backup it is exactly
 * kr_read_file. */
char *kr_read_file_or_backup(const char *path, kr_text_check ok, size_t *len);

/* fflush + fsync: the data is on disk when this returns 0, so renaming the
 * file right after cannot leave it empty after a power cut. */
int kr_flush_sync(FILE *f);

/* kr_text_check: non-empty, no NUL bytes. */
bool kr_text_usable(const char *text, size_t len);

/* kr_text_check: a PEM block that was written to the end. */
bool kr_pem_text_ok(const char *text, size_t len);

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
/* $XDG_CACHE_HOME/kiremote/<rel> (default ~/.cache/kiremote/<rel>). */
bool kr_cache_path(char *out, size_t size, const char *rel);
/* $XDG_RUNTIME_DIR/<name>; without it, /tmp/<name>-<uid>. */
bool kr_runtime_path(char *out, size_t size, const char *name);

/* Is this valid UTF-8 (no overlong forms, no surrogates, up to U+10FFFF)? */
bool kr_utf8_valid(const char *s, size_t len);
/* Latin-1 to UTF-8, into a malloc'd string the caller frees. */
char *kr_latin1_to_utf8(const char *s, size_t len);

#endif
