/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_MCONF_H
#define KR_MCONF_H

/* The [mirror] section of kiremote.ini (the "Mirroring" tab of the
 * settings; the keys are in src/schema.c): what kiremote-mirror does when
 * the command line says nothing. No I/O besides reading the file. */

#include <stdbool.h>

#include "config.h"

typedef struct {
    unsigned max_size; /* 0 = the phone's */
    unsigned bit_rate; /* 0 = the server's */
    unsigned max_fps;  /* 0 = none */
    char codec[8];     /* h264, h265 or av1 */
    bool audio, control, clipboard, screen_off, stay_awake, wifi;
} kr_mirror_conf;

/* Defaults, then whatever cfg (may be NULL) says. A value that makes no
 * sense is logged and ignored, never fatal. */
void kr_mirror_conf_load(kr_mirror_conf *c, const kr_config *cfg);

/* "8M", "500K", "8000000" → bits per second. false for anything else (or
 * zero, or more than a 32-bit count). */
bool kr_parse_bit_rate(const char *s, unsigned *out);

#endif
