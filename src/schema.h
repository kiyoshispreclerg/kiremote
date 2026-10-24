/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_SCHEMA_H
#define KR_SCHEMA_H

/* The settings the interfaces edit: one table that says, for each key of
 * kiremote.ini, what it is, how it is checked, its default and the text
 * that describes it (translated). The daemon serves it ("config-schema",
 * "config-get", "config-set") and both the GTK window and the KiDesktop tab
 * build their screens from it, so a new option shows up in both, and in the
 * CLI, without touching any interface code. Keys are "section.key"; the
 * table is static and its strings are never freed.
 *
 * Not every ini key is here on purpose: [battery] power_supply_dir only
 * exists for the tests. */

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "config.h"

typedef enum {
    KR_S_BOOL,   /* true/false (the ini also reads yes/no/on/off/1/0) */
    KR_S_STRING, /* one line of text */
    KR_S_NAME,   /* the device name: a string the protocol will take */
    KR_S_PATH,   /* absolute, or starting with "~/" */
    KR_S_ENUM,   /* one of choices */
    KR_S_ADDRS,  /* IPv4 addresses, separated by commas */
    KR_S_MAP,    /* a whole section of "name = value" lines ([runcommand]) */
    KR_S_NOTE    /* not a setting: a paragraph the daemon writes, from how things stand (camera.status) */
} kr_setting_type;

typedef struct {
    const char *section;
    const char *key; /* NULL for KR_S_MAP: any key of the section */
    kr_setting_type type;
    const char *def; /* the default as the ini would spell it; "" for none */
    const char *const *choices; /* KR_S_ENUM: NULL-terminated */
    const char *group;          /* id of a group in kr_schema_groups */
    const char *label, *help;   /* msgids (N_), shown translated */
    const char *plugin;         /* only matters while this plugin is on; or NULL */
    bool restart;               /* only read when the daemon starts */
} kr_setting;

typedef struct {
    const char *id, *label; /* label is a msgid */
} kr_setting_group;

/* The table, in the order the screen shows it; n gets its length. */
const kr_setting *kr_schema_all(int *n);
const kr_setting_group *kr_schema_groups(int *n);

/* "section.key" → its entry. A key of a KR_S_MAP section finds the map
 * entry, and *name (if not NULL) points to the name inside spec. NULL if
 * unknown. */
const kr_setting *kr_schema_find(const char *spec, const char **name);

/* Checks value for s (for a map, name is the entry's name, else NULL) and
 * writes it as the ini should hold it to norm ("true", "10.0.0.2, 10.0.0.3").
 * On failure returns false with a translated reason in err. An empty value
 * is fine for every type: it means "back to the default" (the caller
 * unsets the key), and norm is "". */
bool kr_schema_check(const kr_setting *s, const char *name, const char *value, char *norm,
                     size_t norm_size, char *err, size_t err_size);

/* {"settings":[...], "groups":[...]} added to reply: everything an
 * interface needs to draw the screen, translated. */
void kr_schema_to_json(cJSON *reply);

/* {"values":{"section.key":"text"...}, "set":["section.key"...]} added to
 * reply: what each setting is now (the default when the file doesn't say)
 * and which ones the file does set, plus the entries of the map sections. */
void kr_schema_values_to_json(const kr_config *cfg, cJSON *reply);

#endif
