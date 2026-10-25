/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CONFIG_H
#define KR_CONFIG_H

/* kiremote.ini, read with kr_ini so the UI can edit it in place later.
 *
 *   [device]
 *   name = my-computer   # default: the host name, as kdeconnect-kde does
 *   type = desktop       # desktop, laptop, phone, tablet or tv
 *
 *   [network]
 *   addresses = 192.168.0.20, 10.0.0.5   # also announce to these
 *
 *   [share]
 *   dir = ~/Downloads    # received files; default: XDG download folder
 *
 *   [plugins]
 *   battery = false      # any plugin by name; all are on by default
 *
 * kr_config owns its kr_ini and the strings it hands out; they live until
 * kr_config_free. */

#include <stdbool.h>
#include <stddef.h>

typedef struct kr_config kr_config;

/* path NULL means the default (~/.config/kiremote/kiremote.ini). A missing
 * file is not an error: everything takes its default. Problems are logged;
 * NULL only when the file exists but can't be read. */
kr_config *kr_config_load(const char *path);
/* From text, for tests. */
kr_config *kr_config_parse(const char *text);
void kr_config_free(kr_config *c);

const char *kr_config_device_name(const kr_config *c);
/* Always one of the five protocol values. */
const char *kr_config_device_type(const kr_config *c);

/* [network] addresses: IPv4 addresses that get our discovery packet
 * besides the broadcast (for networks that block broadcasts), as written;
 * NULL if none. */
const char *kr_config_addresses(const kr_config *c);

/* [share] dir, with a leading "~/" expanded, into out; else the XDG
 * download folder (user-dirs.dirs), else ~/Downloads. False if there is
 * no home either. */
bool kr_config_share_dir(const kr_config *c, char *out, size_t size);

/* [plugins] <name>: false/no/off/0 turns a plugin off, true/yes/on/1 on;
 * without the line, fallback. */
bool kr_config_plugin_enabled(const kr_config *c, const char *name, bool fallback);

/* A string, bool or int setting of any section, for plugins; the string is
 * owned by c. */
const char *kr_config_get(const kr_config *c, const char *section, const char *key);
bool kr_config_get_bool(const kr_config *c, const char *section, const char *key, bool fallback);

/* A path setting, with a leading "~/" expanded, into out. False if the
 * setting is empty or not absolute. */
bool kr_config_get_path(const kr_config *c, const char *section, const char *key, char *out, size_t size);

/* Walks the "key = value" lines of a section in file order: start with
 * *i = 0; false at the end. Strings are owned by c. */
bool kr_config_each(const kr_config *c, const char *section, int *i, const char **key, const char **value);

/* Editing, for the interfaces (through the daemon's control socket). The
 * file keeps its comments and order; only the touched lines change. Strings
 * handed out earlier by this config (kr_config_get...) are invalid after a
 * set or unset, so nobody keeps them across turns of the loop. False
 * without memory. */
bool kr_config_set(kr_config *c, const char *section, const char *key, const char *value);
void kr_config_unset(kr_config *c, const char *section, const char *key);
/* Writes the file atomically; false (logged) if it cannot, or if the config
 * came from kr_config_parse and has no file. */
bool kr_config_save(const kr_config *c);

/* True for the device types the protocol knows. "smartphone" is the old
 * spelling of "phone" and is accepted on input. */
bool kr_device_type_valid(const char *type);

#endif
