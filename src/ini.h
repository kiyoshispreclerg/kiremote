/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_INI_H
#define KR_INI_H

#include <stdbool.h>
#include <stddef.h>

/* A small INI document that can be read and edited in place:
 *
 *   # comment            ; comment too
 *   [section]
 *   key = value
 *
 * Every line is kept as written, so a file edited by the kiremote UI keeps
 * the user's comments, order and blank lines; only the touched lines change.
 * Section and key names are case-insensitive. Values are trimmed; there is
 * no quoting or escaping (a value may contain "=", "#" and ";"). */

typedef struct kr_ini kr_ini;

/* Empty document. */
kr_ini *kr_ini_new(void);
/* Parses text. Malformed lines are kept (and skipped by the getters); each
 * one is reported to warn with its 1-based line number, if warn is set. */
/* kr_text_check for a config file: "usable" is anything non-empty without NUL
 * bytes (what a file truncated or zeroed by a power cut is not); "clean" also
 * parses without a single warning, the bar for becoming the backup. */
bool kr_ini_text_usable(const char *text, size_t len);
bool kr_ini_text_clean(const char *text, size_t len);

kr_ini *kr_ini_parse(const char *text,
                     void (*warn)(void *ud, int line, const char *msg),
                     void *ud);
void kr_ini_free(kr_ini *ini);

/* NULL if absent. With repeated keys, the last one wins. */
const char *kr_ini_get(const kr_ini *ini, const char *section, const char *key);
/* Line number of that key (0 if absent), for messages. */
int kr_ini_line(const kr_ini *ini, const char *section, const char *key);

/* Iteration in file order. Returns false at the end. *section is "" for keys
 * before the first header. Pass i = 0 first, then what it leaves in *i. */
bool kr_ini_next(const kr_ini *ini, int *i, const char **section,
                 const char **key, const char **value, int *line);

/* Sections in file order, each once; same protocol as kr_ini_next. */
bool kr_ini_next_section(const kr_ini *ini, int *i, const char **section);

/* Replaces the value in place, or appends the key to its section (created at
 * the end if missing). */
int kr_ini_set(kr_ini *ini, const char *section, const char *key,
               const char *value);
/* Removes every line with that key. */
void kr_ini_unset(kr_ini *ini, const char *section, const char *key);
/* Removes the section header and its keys (comments inside go too). */
void kr_ini_remove_section(kr_ini *ini, const char *section);

/* The whole document as text; the caller frees it. */
char *kr_ini_dump(const kr_ini *ini);
/* Writes atomically (temp file + rename), creating the parent folders. */
int kr_ini_save(const kr_ini *ini, const char *path);

#endif
