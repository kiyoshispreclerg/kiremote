/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Phase 0 building blocks: paths, configuration, ini editing, JSON paths. */
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "config.h"
#include "ini.h"
#include "json.h"
#include "transfer.h"
#include "util.h"

TEST(pathf_refuses_to_truncate)
{
    char buf[8];
    CHECK(kr_pathf(buf, sizeof buf, "%s", "1234567"));
    CHECK_STR_EQ(buf, "1234567");
    CHECK(!kr_pathf(buf, sizeof buf, "%s", "12345678"));
    CHECK_STR_EQ(buf, "");
}

TEST(config_path_follows_xdg)
{
    char buf[KR_PATH_MAX];
    setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
    CHECK(kr_config_path(buf, sizeof buf, "kiremote.ini"));
    CHECK_STR_EQ(buf, "/x/cfg/kiremote/kiremote.ini");

    /* Relative values are ignored, per the XDG spec. */
    setenv("XDG_CONFIG_HOME", "rel", 1);
    setenv("HOME", "/home/u", 1);
    CHECK(kr_config_path(buf, sizeof buf, "a"));
    CHECK_STR_EQ(buf, "/home/u/.config/kiremote/a");
    unsetenv("XDG_CONFIG_HOME");
}

TEST(runtime_path)
{
    char buf[KR_PATH_MAX];
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    CHECK(kr_runtime_path(buf, sizeof buf, "kiremote.sock"));
    CHECK_STR_EQ(buf, "/run/user/1000/kiremote.sock");
}

TEST(config_defaults)
{
    kr_config *c = kr_config_parse("");
    CHECK(c);
    CHECK(kr_config_device_name(c)[0] != '\0');
    CHECK_STR_EQ(kr_config_device_type(c), "desktop");
    kr_config_free(c);
}

TEST(config_values)
{
    kr_config *c = kr_config_parse("[device]\nname = Escritório\ntype = laptop\n");
    CHECK(c);
    CHECK_STR_EQ(kr_config_device_name(c), "Escritório");
    CHECK_STR_EQ(kr_config_device_type(c), "laptop");
    kr_config_free(c);

    /* "smartphone" is the old spelling; unknown types fall back. */
    c = kr_config_parse("[device]\ntype = smartphone\n");
    CHECK_STR_EQ(kr_config_device_type(c), "phone");
    kr_config_free(c);
    c = kr_config_parse("[device]\ntype = toaster\n");
    CHECK_STR_EQ(kr_config_device_type(c), "desktop");
    kr_config_free(c);
}

TEST(ini_edit_keeps_comments)
{
    kr_ini *ini = kr_ini_parse("# top\n[device]\nname = a # not a comment\n", NULL, NULL);
    CHECK(ini);
    CHECK_STR_EQ(kr_ini_get(ini, "DEVICE", "Name"), "a # not a comment");
    CHECK(kr_ini_set(ini, "device", "name", "b") == 0);
    CHECK(kr_ini_set(ini, "plugins", "ping", "true") == 0);
    char *s = kr_ini_dump(ini);
    CHECK_STR_EQ(s, "# top\n[device]\nname = b\n\n[plugins]\nping = true\n");
    free(s);
    kr_ini_free(ini);
}

TEST(json_paths)
{
    cJSON *j = cJSON_Parse("{\"type\":\"kdeconnect.identity\","
                           "\"body\":{\"tcpPort\":1716,\"caps\":[\"a\",\"b\"]}}");
    CHECK(j);
    CHECK_STR_EQ(kr_json_str(j, "type"), "kdeconnect.identity");
    CHECK_INT_EQ(kr_json_num(j, "body.tcpPort", 0), 1716);
    CHECK_STR_EQ(kr_json_str(j, "body.caps[-1]"), "b");
    CHECK(kr_json_path(j, "body.missing") == NULL);
    cJSON_Delete(j);
}

TEST(unique_file_names)
{
    char buf[KR_PATH_MAX];
    CHECK(kr_unique_path("/d/foto.jpg", 0, buf, sizeof buf));
    CHECK_STR_EQ(buf, "/d/foto.jpg");
    CHECK(kr_unique_path("/d/foto.jpg", 2, buf, sizeof buf));
    CHECK_STR_EQ(buf, "/d/foto (2).jpg");
    CHECK(kr_unique_path("/d.x/notas", 1, buf, sizeof buf));
    CHECK_STR_EQ(buf, "/d.x/notas (1)");
    CHECK(kr_unique_path("/d/.bashrc", 1, buf, sizeof buf));
    CHECK_STR_EQ(buf, "/d/.bashrc (1)");
}

TEST(share_dir)
{
    char buf[KR_PATH_MAX];
    setenv("HOME", "/home/u", 1);
    setenv("XDG_CONFIG_HOME", "/nonexistent", 1);
    kr_config *c = kr_config_parse("[share]\ndir = ~/Recebidos\n");
    CHECK(kr_config_share_dir(c, buf, sizeof buf));
    CHECK_STR_EQ(buf, "/home/u/Recebidos");
    kr_config_free(c);
    /* No setting and no user-dirs.dirs: ~/Downloads. */
    c = kr_config_parse("");
    CHECK(kr_config_share_dir(c, buf, sizeof buf));
    CHECK_STR_EQ(buf, "/home/u/Downloads");
    CHECK(kr_config_addresses(c) == NULL);
    kr_config_free(c);
    c = kr_config_parse("[network]\naddresses = 10.0.0.2\n");
    CHECK_STR_EQ(kr_config_addresses(c), "10.0.0.2");
    kr_config_free(c);
    unsetenv("XDG_CONFIG_HOME");
}

int main(void)
{
    RUN(pathf_refuses_to_truncate);
    RUN(config_path_follows_xdg);
    RUN(runtime_path);
    RUN(config_defaults);
    RUN(config_values);
    RUN(ini_edit_keeps_comments);
    RUN(json_paths);
    RUN(unique_file_names);
    RUN(share_dir);
    return harness_report();
}
