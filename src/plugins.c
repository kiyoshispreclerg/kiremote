/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "plugin.h"

extern const kr_plugin kr_plugin_ping;
extern const kr_plugin kr_plugin_share;

const kr_plugin *const kr_plugins[] = {
    &kr_plugin_ping,
    &kr_plugin_share,
    NULL,
};

static bool listed(const char *const *types, const char *type)
{
    for (; types && *types; types++)
        if (strcmp(*types, type) == 0)
            return true;
    return false;
}

cJSON *kr_plugins_capabilities(bool incoming)
{
    cJSON *a = cJSON_CreateArray();
    for (const kr_plugin *const *p = kr_plugins; a && *p; p++)
        for (const char *const *t = incoming ? (*p)->incoming : (*p)->outgoing; t && *t; t++) {
            bool dup = false;
            const cJSON *e;
            cJSON_ArrayForEach(e, a) dup |= strcmp(e->valuestring, *t) == 0;
            if (!dup)
                cJSON_AddItemToArray(a, cJSON_CreateString(*t));
        }
    return a;
}

const kr_plugin *kr_plugin_for(const char *type)
{
    for (const kr_plugin *const *p = kr_plugins; *p; p++)
        if (listed((*p)->incoming, type))
            return *p;
    return NULL;
}
