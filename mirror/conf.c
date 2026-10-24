/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "conf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "log.h"
#include "stream.h"

bool kr_parse_bit_rate(const char *s, unsigned *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *s == '-' || *s == '+' || v == 0)
        return false;
    if (*end == 'k' || *end == 'K') {
        v *= 1000;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        v *= 1000000;
        end++;
    }
    if (*end || v > 0xFFFFFFFFull)
        return false;
    *out = (unsigned)v;
    return true;
}

static bool parse_count(const char *s, unsigned max, unsigned *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end || v == 0 || v > max)
        return false;
    *out = (unsigned)v;
    return true;
}

static void bad(const char *key, const char *value)
{
    kr_log_warn(_("kiremote.ini: [mirror] %s = %s is not valid, using the default"), key, value);
}

void kr_mirror_conf_load(kr_mirror_conf *c, const kr_config *cfg)
{
    *c = (kr_mirror_conf){.audio = true, .control = true, .clipboard = true, .wifi = true};
    snprintf(c->codec, sizeof c->codec, "h264");
    if (!cfg)
        return;
    const char *v;
    if ((v = kr_config_get(cfg, "mirror", "max_size")) && *v && !parse_count(v, KR_STREAM_MAX_SIDE, &c->max_size))
        bad("max_size", v);
    if ((v = kr_config_get(cfg, "mirror", "bit_rate")) && *v && !kr_parse_bit_rate(v, &c->bit_rate))
        bad("bit_rate", v);
    if ((v = kr_config_get(cfg, "mirror", "max_fps")) && *v && !parse_count(v, 1000, &c->max_fps))
        bad("max_fps", v);
    if ((v = kr_config_get(cfg, "mirror", "codec")) && *v) {
        if (!strcmp(v, "h264") || !strcmp(v, "h265") || !strcmp(v, "av1"))
            snprintf(c->codec, sizeof c->codec, "%s", v);
        else
            bad("codec", v);
    }
    c->audio = kr_config_get_bool(cfg, "mirror", "audio", c->audio);
    c->control = kr_config_get_bool(cfg, "mirror", "control", c->control);
    c->clipboard = kr_config_get_bool(cfg, "mirror", "clipboard", c->clipboard);
    c->screen_off = kr_config_get_bool(cfg, "mirror", "screen_off", c->screen_off);
    c->stay_awake = kr_config_get_bool(cfg, "mirror", "stay_awake", c->stay_awake);
    c->wifi = kr_config_get_bool(cfg, "mirror", "wifi", c->wifi);
}
