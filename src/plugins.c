/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "plugin.h"

extern const kr_plugin kr_plugin_ping;
extern const kr_plugin kr_plugin_share;

const kr_plugin *const kr_plugins[] = {
    &kr_plugin_ping,
    &kr_plugin_share,
    &kr_plugin_battery,
    &kr_plugin_connectivity,
    &kr_plugin_find,
    &kr_plugin_telephony,
    &kr_plugin_pausemusic,
    &kr_plugin_mpris,
    &kr_plugin_volume,
    NULL,
};

static bool listed(const char *const *types, const char *type)
{
    for (; types && *types; types++)
        if (strcmp(*types, type) == 0)
            return true;
    return false;
}

cJSON *kr_plugins_capabilities(const kr_config *cfg, bool incoming)
{
    cJSON *a = cJSON_CreateArray();
    for (const kr_plugin *const *p = kr_plugins; a && *p; p++) {
        if (!kr_config_plugin_enabled(cfg, (*p)->name))
            continue;
        for (const char *const *t = incoming ? (*p)->incoming : (*p)->outgoing; t && *t; t++) {
            bool dup = false;
            const cJSON *e;
            cJSON_ArrayForEach(e, a) dup |= strcmp(e->valuestring, *t) == 0;
            if (!dup)
                cJSON_AddItemToArray(a, cJSON_CreateString(*t));
        }
    }
    return a;
}

bool kr_plugin_takes(const kr_plugin *pl, const char *type)
{
    return listed(pl->incoming, type);
}
