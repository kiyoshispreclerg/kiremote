/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The settings table the interfaces are built from, and editing the config
 * through it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "cJSON.h"
#include "config.h"
#include "plugin.h"
#include "schema.h"
#include "util.h"

static bool check(const char *spec, const char *value, char *norm, char *err)
{
    const char *name;
    const kr_setting *s = kr_schema_find(spec, &name);
    if (!s)
        return false;
    return kr_schema_check(s, name, value, norm, 1100, err, 160);
}

/* Both interfaces draw their "Features" page from this table: a plugin
 * without a line would be impossible to turn off from either. */
TEST(every_plugin_has_a_switch)
{
    for (const kr_plugin *const *p = kr_plugins; *p; p++) {
        char spec[96];
        snprintf(spec, sizeof spec, "plugins.%s", (*p)->name);
        const kr_setting *s = kr_schema_find(spec, NULL);
        CHECK_MSG(s, "no switch for plugin %s", (*p)->name);
        CHECK_INT_EQ(s->type, KR_S_BOOL);
        CHECK_STR_EQ(s->def, (*p)->off_by_default ? "false" : "true");
    }
}

TEST(table_is_consistent)
{
    int n, ng;
    const kr_setting *all = kr_schema_all(&n);
    const kr_setting_group *groups = kr_schema_groups(&ng);
    for (int i = 0; i < n; i++) {
        const kr_setting *s = &all[i];
        bool group_ok = false;
        for (int g = 0; g < ng; g++)
            group_ok |= strcmp(groups[g].id, s->group) == 0;
        CHECK_MSG(group_ok, "%s.%s: unknown group %s", s->section, s->key ? s->key : "*", s->group);
        CHECK(s->label && s->help);
        CHECK((s->type == KR_S_MAP) == (s->key == NULL));
        CHECK((s->type == KR_S_ENUM) == (s->choices != NULL));

        if (s->plugin) {
            bool known = false;
            for (const kr_plugin *const *p = kr_plugins; *p; p++)
                known |= strcmp((*p)->name, s->plugin) == 0;
            CHECK_MSG(known, "%s.%s: unknown plugin %s", s->section, s->key ? s->key : "*", s->plugin);
        }
        /* Its own default must be acceptable (or empty: none). */
        if (s->key && s->def[0]) {
            char spec[96], norm[1100], err[160];
            snprintf(spec, sizeof spec, "%s.%s", s->section, s->key);
            CHECK_MSG(check(spec, s->def, norm, err), "%s: default \"%s\": %s", spec, s->def, err);
            CHECK_STR_EQ(norm, s->def);
        }
        for (int j = 0; j < i; j++)
            CHECK(!(all[j].key && s->key && strcmp(all[j].section, s->section) == 0 &&
                    strcmp(all[j].key, s->key) == 0));
    }
}

TEST(find_by_spec)
{
    const char *name = "x";
    CHECK(kr_schema_find("device.name", &name));
    CHECK(name == NULL);
    CHECK(kr_schema_find("DEVICE.Name", NULL));
    CHECK(!kr_schema_find("device", NULL));
    CHECK(!kr_schema_find("device.", NULL));
    CHECK(!kr_schema_find(".name", NULL));
    CHECK(!kr_schema_find("device.nope", NULL));
    CHECK(!kr_schema_find("battery.power_supply_dir", NULL)); /* tests only */

    const kr_setting *m = kr_schema_find("runcommand.Lock screen", &name);
    CHECK(m && m->type == KR_S_MAP);
    CHECK_STR_EQ(name, "Lock screen");
}

TEST(check_bool)
{
    char norm[1100], err[160];
    CHECK(check("clipboard.auto_share", "off", norm, err));
    CHECK_STR_EQ(norm, "false");
    CHECK(check("clipboard.auto_share", " YES ", norm, err));
    CHECK_STR_EQ(norm, "true");
    CHECK(!check("clipboard.auto_share", "maybe", norm, err));
    CHECK(err[0]);
}

TEST(check_color)
{
    char norm[1100], err[160];
    CHECK(check("pointer.color", "#FF8000", norm, err));
    CHECK_STR_EQ(norm, "#ff8000");
    CHECK(!check("pointer.color", "red", norm, err));
    CHECK(!check("pointer.color", "#ff80", norm, err));
    CHECK(!check("pointer.color", "#gg0000", norm, err));
    CHECK(!check("pointer.outline_color", "ff0000", norm, err));
    CHECK(check("pointer.shape", "diamond", norm, err));
    CHECK(!check("pointer.shape", "star", norm, err));
    CHECK(check("pointer.hide_after", "0", norm, err));
    CHECK(!check("pointer.hide_after", "3", norm, err));
}

TEST(check_empty_means_default)
{
    char norm[1100], err[160];
    CHECK(check("clipboard.auto_share", "", norm, err));
    CHECK_STR_EQ(norm, "");
    CHECK(check("device.type", "  ", norm, err));
    CHECK_STR_EQ(norm, "");
    CHECK(check("runcommand.Lock", "", norm, err));
}

TEST(check_enum)
{
    char norm[1100], err[160];
    CHECK(check("device.type", "laptop", norm, err));
    CHECK_STR_EQ(norm, "laptop");
    CHECK(!check("device.type", "toaster", norm, err));
    CHECK(!check("device.type", "smartphone", norm, err));
    CHECK(check("find.player", "pw-play", norm, err));
}

TEST(check_addresses)
{
    char norm[1100], err[160];
    CHECK(check("network.addresses", "10.0.0.2,192.168.0.20  10.0.0.3", norm, err));
    CHECK_STR_EQ(norm, "10.0.0.2, 192.168.0.20, 10.0.0.3");
    CHECK(!check("network.addresses", "10.0.0.2, nope", norm, err));
    CHECK(!check("network.addresses", "::1", norm, err));
    CHECK(!check("network.addresses", "10.0.0.256", norm, err));
    CHECK(!check("network.addresses", ",", norm, err));
    /* Seventeen are too many. */
    char many[400] = "";
    for (int i = 1; i <= 17; i++)
        snprintf(many + strlen(many), sizeof many - strlen(many), "10.0.0.%d ", i);
    CHECK(!check("network.addresses", many, norm, err));
}

TEST(check_paths_and_text)
{
    char norm[1100], err[160];
    CHECK(check("share.dir", "~/Recebidos", norm, err));
    CHECK_STR_EQ(norm, "~/Recebidos");
    CHECK(check("share.dir", "/srv/x", norm, err));
    CHECK(!check("share.dir", "relative/dir", norm, err));
    CHECK(!check("find.sound", "x.ogg", norm, err));
    CHECK(!check("lock.command", "a\nb", norm, err));
    CHECK(!check("lock.command", "a\rb", norm, err));
    CHECK(check("lock.command", "  loginctl lock-session  ", norm, err));
    CHECK_STR_EQ(norm, "loginctl lock-session");
    char big[1100];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CHECK(!check("lock.command", big, norm, err));
}

TEST(check_device_name)
{
    char norm[1100], err[160];
    CHECK(check("device.name", "Escritório", norm, err));
    CHECK_STR_EQ(norm, "Escritório");
    CHECK(!check("device.name", "\"\"", norm, err)); /* the protocol drops everything */
}

/* A map key becomes a line of the ini: it must not turn into a comment,
 * a section header or two keys. */
TEST(check_map_names)
{
    char norm[1100], err[160];
    CHECK(check("runcommand.Lock screen", "loginctl lock-session", norm, err));
    CHECK_STR_EQ(norm, "loginctl lock-session");
    CHECK(check("runcommand.a.b", "x", norm, err)); /* dots are fine after the section */
    CHECK(!check("runcommand.#x", "x", norm, err));
    CHECK(!check("runcommand.;x", "x", norm, err));
    CHECK(!check("runcommand.[x", "x", norm, err));
    CHECK(!check("runcommand.a=b", "x", norm, err));
    CHECK(!check("runcommand.a\nb", "x", norm, err));
    CHECK(!check("runcommand. x", "x", norm, err));
    CHECK(!check("runcommand.x ", "x", norm, err));
    char longname[100];
    snprintf(longname, sizeof longname, "runcommand.%065d", 0);
    CHECK(!check(longname, "x", norm, err));
}

TEST(schema_json)
{
    cJSON *r = cJSON_CreateObject();
    kr_schema_to_json(r);
    const cJSON *settings = cJSON_GetObjectItem(r, "settings");
    int n;
    kr_schema_all(&n);
    CHECK_INT_EQ(cJSON_GetArraySize(settings), n);
    const cJSON *e;
    bool saw_enum = false, saw_map = false;
    cJSON_ArrayForEach(e, settings)
    {
        CHECK(cJSON_IsString(cJSON_GetObjectItem(e, "key")));
        CHECK(cJSON_IsString(cJSON_GetObjectItem(e, "label")));
        CHECK(cJSON_IsString(cJSON_GetObjectItem(e, "help")));
        CHECK(cJSON_IsString(cJSON_GetObjectItem(e, "group")));
        CHECK(cJSON_IsBool(cJSON_GetObjectItem(e, "restart")));
        const cJSON *type = cJSON_GetObjectItem(e, "type");
        if (strcmp(type->valuestring, "enum") == 0) {
            const cJSON *ch = cJSON_GetObjectItem(e, "choices"), *lb = cJSON_GetObjectItem(e, "choice_labels");
            CHECK(cJSON_IsArray(ch) && cJSON_IsArray(lb));
            CHECK_INT_EQ(cJSON_GetArraySize(ch), cJSON_GetArraySize(lb));
            saw_enum = true;
        }
        saw_map |= strcmp(type->valuestring, "map") == 0 &&
                   strcmp(cJSON_GetObjectItem(e, "key")->valuestring, "runcommand.*") == 0;
    }
    CHECK(saw_enum && saw_map);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(r, "groups")) > 3);
    cJSON_Delete(r);
}

static bool in_set(const cJSON *set, const char *key)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, set) if (strcmp(e->valuestring, key) == 0) return true;
    return false;
}

TEST(values_json)
{
    setenv("HOME", "/home/u", 1);
    unsetenv("XDG_CONFIG_HOME");
    kr_config *c = kr_config_parse("[device]\nname = Mesa\n[plugins]\nbattery = no\n"
                                   "[clipboard]\nauto_share = off\n"
                                   "[runcommand]\nOla = echo oi\n");
    cJSON *r = cJSON_CreateObject();
    kr_schema_values_to_json(c, r);
    const cJSON *v = cJSON_GetObjectItem(r, "values"), *set = cJSON_GetObjectItem(r, "set");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "device.name")->valuestring, "Mesa");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "device.type")->valuestring, "desktop");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "plugins.battery")->valuestring, "false");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "plugins.ping")->valuestring, "true");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "plugins.screensaver_inhibit")->valuestring, "false");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "clipboard.auto_share")->valuestring, "false");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "clipboard.share_passwords")->valuestring, "false");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "pausemusic.resume")->valuestring, "true");
    CHECK_STR_EQ(cJSON_GetObjectItem(v, "runcommand.Ola")->valuestring, "echo oi");
    /* The default share folder, spelled out. */
    CHECK_STR_HAS(cJSON_GetObjectItem(v, "share.dir")->valuestring, "/home/u");

    CHECK(in_set(set, "device.name"));
    CHECK(in_set(set, "plugins.battery"));
    CHECK(in_set(set, "runcommand.Ola"));
    CHECK(!in_set(set, "device.type"));
    CHECK(!in_set(set, "plugins.ping"));
    cJSON_Delete(r);
    kr_config_free(c);
}

TEST(every_setting_has_a_value)
{
    kr_config *c = kr_config_parse("");
    cJSON *r = cJSON_CreateObject();
    kr_schema_values_to_json(c, r);
    const cJSON *v = cJSON_GetObjectItem(r, "values");
    int n;
    const kr_setting *all = kr_schema_all(&n);
    for (int i = 0; i < n; i++) {
        if (!all[i].key)
            continue;
        char spec[96];
        snprintf(spec, sizeof spec, "%s.%s", all[i].section, all[i].key);
        CHECK_MSG(cJSON_IsString(cJSON_GetObjectItem(v, spec)), "no value for %s", spec);
    }
    CHECK_INT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(r, "set")), 0);
    cJSON_Delete(r);
    kr_config_free(c);
}

/* ---- editing --------------------------------------------------------------- */

TEST(config_set_keeps_derived_values_fresh)
{
    kr_config *c = kr_config_parse("[device]\nname = Velho\n");
    CHECK_STR_EQ(kr_config_device_name(c), "Velho");
    CHECK(kr_config_set(c, "device", "name", "Novo nome"));
    CHECK_STR_EQ(kr_config_device_name(c), "Novo nome");
    CHECK(kr_config_set(c, "device", "type", "tv"));
    CHECK_STR_EQ(kr_config_device_type(c), "tv");
    kr_config_unset(c, "device", "name");
    CHECK(kr_config_device_name(c)[0] != '\0'); /* the host name */
    CHECK(strcmp(kr_config_device_name(c), "Novo nome") != 0);
    kr_config_unset(c, "device", "type");
    CHECK_STR_EQ(kr_config_device_type(c), "desktop");
    kr_config_free(c);
}

TEST(config_save_needs_a_file)
{
    kr_config *c = kr_config_parse("");
    CHECK(!kr_config_save(c));
    kr_config_free(c);
}

TEST(config_save_keeps_comments)
{
    char dir[] = "/tmp/krcfg.XXXXXX";
    CHECK(mkdtemp(dir));
    char path[KR_PATH_MAX];
    snprintf(path, sizeof path, "%s/sub/kiremote.ini", dir);
    kr_make_parent_dirs(path);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fputs("# mine\n[device]\n# the name\nname = A\n\n[plugins]\nping = false\n", f);
    fclose(f);

    kr_config *c = kr_config_load(path);
    CHECK(c);
    CHECK(kr_config_set(c, "device", "name", "B"));
    CHECK(kr_config_set(c, "share", "dir", "/srv"));
    kr_config_unset(c, "plugins", "ping");
    CHECK(kr_config_save(c));
    kr_config_free(c);

    char *text = kr_read_file(path, NULL);
    CHECK(text);
    CHECK_STR_HAS(text, "# mine\n");
    CHECK_STR_HAS(text, "# the name\nname = B\n");
    CHECK_STR_HAS(text, "[share]\ndir = /srv\n");
    CHECK(!strstr(text, "ping"));
    free(text);

    /* A fresh load sees it. */
    c = kr_config_load(path);
    CHECK_STR_EQ(kr_config_device_name(c), "B");
    kr_config_free(c);

    /* A file that was not there is created, with its folders. */
    snprintf(path, sizeof path, "%s/new/dir/kiremote.ini", dir);
    c = kr_config_load(path);
    CHECK(c);
    CHECK(kr_config_set(c, "device", "type", "tablet"));
    CHECK(kr_config_save(c));
    kr_config_free(c);
    CHECK(kr_file_exists(path));

    char cmd[KR_PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

/* The Camera tab's first line is not a setting: the daemon writes it, from how
 * things stand, and nobody can set it. */
TEST(the_camera_note_is_read_only_and_alive)
{
    const kr_setting *s = kr_schema_find("camera.status", NULL);
    CHECK(s && s->type == KR_S_NOTE);
    char norm[64], err[160];
    CHECK(!check("camera.status", "anything", norm, err));
    CHECK_STR_HAS(err, "not a setting");

    kr_config *c = kr_config_parse("[camera]\ndevice = /nonexistent/video9\n");
    cJSON *r = cJSON_CreateObject();
    kr_schema_values_to_json(c, r);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "values"), "camera.status");
    CHECK(cJSON_IsString(v));
    CHECK_STR_HAS(v->valuestring, "Cannot use /nonexistent/video9");
    CHECK(!cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "set"), "camera.status"));
    cJSON_Delete(r);
    kr_config_free(c);
}

int main(void)
{
    RUN(every_plugin_has_a_switch);
    RUN(table_is_consistent);
    RUN(find_by_spec);
    RUN(check_bool);
    RUN(check_color);
    RUN(check_empty_means_default);
    RUN(check_enum);
    RUN(check_addresses);
    RUN(check_paths_and_text);
    RUN(check_device_name);
    RUN(check_map_names);
    RUN(schema_json);
    RUN(values_json);
    RUN(every_setting_has_a_value);
    RUN(config_set_keeps_derived_values_fresh);
    RUN(config_save_needs_a_file);
    RUN(config_save_keeps_comments);
    RUN(the_camera_note_is_read_only_and_alive);
    return harness_report();
}
