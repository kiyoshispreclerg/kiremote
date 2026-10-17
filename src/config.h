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

/* True for the device types the protocol knows. "smartphone" is the old
 * spelling of "phone" and is accepted on input. */
bool kr_device_type_valid(const char *type);

#endif
