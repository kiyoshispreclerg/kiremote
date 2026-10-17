/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "i18n.h"
#include "ini.h"
#include "log.h"
#include "util.h"

struct kr_config {
    kr_ini *ini;
    char hostname[256];
    const char *name;
    const char *type;
};

bool kr_device_type_valid(const char *type)
{
    static const char *const types[] = {"desktop", "laptop", "phone", "smartphone",
                                        "tablet", "tv"};
    for (size_t i = 0; i < sizeof types / sizeof *types; i++)
        if (strcmp(type, types[i]) == 0)
            return true;
    return false;
}

static void warn_line(void *ud, int line, const char *msg)
{
    kr_log_warn("%s:%d: %s", (const char *)ud, line, msg);
}

static kr_config *from_ini(kr_ini *ini, const char *where)
{
    kr_config *c = calloc(1, sizeof *c);
    if (!c) {
        kr_ini_free(ini);
        return NULL;
    }
    c->ini = ini;

    const char *name = kr_ini_get(ini, "device", "name");
    if (name && *name) {
        c->name = name;
    } else {
        if (gethostname(c->hostname, sizeof c->hostname) != 0 || !c->hostname[0])
            kr_pathf(c->hostname, sizeof c->hostname, "kiremote");
        c->hostname[sizeof c->hostname - 1] = '\0';
        c->name = c->hostname;
    }

    const char *type = kr_ini_get(ini, "device", "type");
    c->type = "desktop";
    if (type && *type) {
        if (!kr_device_type_valid(type))
            kr_log_warn(_("%s:%d: unknown device type \"%s\", using \"desktop\""), where,
                        kr_ini_line(ini, "device", "type"), type);
        else
            c->type = strcmp(type, "smartphone") == 0 ? "phone" : type;
    }
    return c;
}

kr_config *kr_config_parse(const char *text)
{
    kr_ini *ini = kr_ini_parse(text, warn_line, (void *)"(text)");
    return ini ? from_ini(ini, "(text)") : NULL;
}

kr_config *kr_config_load(const char *path)
{
    char def[KR_PATH_MAX];
    if (!path) {
        if (!kr_config_path(def, sizeof def, "kiremote.ini")) {
            kr_log_error(_("cannot find the configuration folder (is HOME set?)"));
            return NULL;
        }
        path = def;
    }

    char *text = kr_read_file(path, NULL);
    if (!text) {
        if (errno != ENOENT) {
            kr_log_error(_("cannot read %s: %s"), path, strerror(errno));
            return NULL;
        }
        kr_log_debug("%s not found, using defaults", path);
        kr_ini *ini = kr_ini_new();
        return ini ? from_ini(ini, path) : NULL;
    }
    kr_ini *ini = kr_ini_parse(text, warn_line, (void *)path);
    free(text);
    return ini ? from_ini(ini, path) : NULL;
}

void kr_config_free(kr_config *c)
{
    if (!c)
        return;
    kr_ini_free(c->ini);
    free(c);
}

const char *kr_config_device_name(const kr_config *c)
{
    return c->name;
}

const char *kr_config_device_type(const kr_config *c)
{
    return c->type;
}

const char *kr_config_addresses(const kr_config *c)
{
    const char *v = kr_ini_get(c->ini, "network", "addresses");
    return v && *v ? v : NULL;
}

/* "~/x" → "$HOME/x"; "$HOME/x" (user-dirs.dirs) too. */
static bool expand_home(const char *in, char *out, size_t size)
{
    const char *home = getenv("HOME");
    if (strncmp(in, "~/", 2) == 0 || strcmp(in, "~") == 0)
        return home && kr_pathf(out, size, "%s%s", home, in + 1);
    if (strncmp(in, "$HOME", 5) == 0 && (in[5] == '/' || !in[5]))
        return home && kr_pathf(out, size, "%s%s", home, in + 5);
    return in[0] == '/' && kr_pathf(out, size, "%s", in);
}

/* XDG_DOWNLOAD_DIR="..." from $XDG_CONFIG_HOME/user-dirs.dirs. */
static bool xdg_download_dir(char *out, size_t size)
{
    char path[KR_PATH_MAX];
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    bool ok = cfg && cfg[0] == '/' ? kr_pathf(path, sizeof path, "%s/user-dirs.dirs", cfg)
                                   : home && kr_pathf(path, sizeof path, "%s/.config/user-dirs.dirs", home);
    char *text = ok ? kr_read_file(path, NULL) : NULL;
    if (!text)
        return false;
    bool found = false;
    for (char *line = strtok(text, "\n"); line && !found; line = strtok(NULL, "\n")) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (strncmp(line, "XDG_DOWNLOAD_DIR=\"", 18) != 0)
            continue;
        char *v = line + 18, *end = strchr(v, '"');
        if (end) {
            *end = '\0';
            found = expand_home(v, out, size);
        }
    }
    free(text);
    return found;
}

bool kr_config_share_dir(const kr_config *c, char *out, size_t size)
{
    const char *v = kr_ini_get(c->ini, "share", "dir");
    if (v && *v && expand_home(v, out, size))
        return true;
    if (xdg_download_dir(out, size))
        return true;
    const char *home = getenv("HOME");
    return home && home[0] == '/' && kr_pathf(out, size, "%s/Downloads", home);
}
