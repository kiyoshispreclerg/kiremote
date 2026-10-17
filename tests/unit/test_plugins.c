/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The pure parts of the phase 2 plugins: the system battery, pactl's sink
 * list, the ring sound, plugin switches. The rest is in tests/net/plugins.sh. */
#define _DEFAULT_SOURCE /* mkdtemp */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../harness.h"
#include "config.h"
#include "packet.h"
#include "plugin.h"
#include "util.h"

static char dir[64];

static void put(const char *sub, const char *file, const char *text)
{
    char path[KR_PATH_MAX];
    kr_pathf(path, sizeof path, "%s/%s", dir, sub);
    mkdir(path, 0755);
    kr_pathf(path, sizeof path, "%s/%s/%s", dir, sub, file);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void make_dir(void)
{
    snprintf(dir, sizeof dir, "/tmp/kiremote-test-XXXXXX");
    if (!mkdtemp(dir))
        abort();
}

static void rm_rf(void)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
}

TEST(battery_from_sysfs)
{
    make_dir();
    kr_battery_info b;
    CHECK(!kr_battery_read(dir, &b)); /* nothing there: a desktop */

    put("AC", "type", "Mains\n");
    put("mouse", "type", "Battery\n");
    put("mouse", "scope", "Device\n");
    put("mouse", "capacity", "5\n");
    CHECK(!kr_battery_read(dir, &b)); /* a wireless mouse is not this computer */

    put("BAT0", "type", "Battery\n");
    put("BAT0", "capacity", "80\n");
    put("BAT0", "status", "Discharging\n");
    CHECK(kr_battery_read(dir, &b));
    CHECK_INT_EQ(b.count, 1);
    CHECK_INT_EQ(b.charge, 80);
    CHECK(b.discharging && !b.charging);

    /* Two batteries average out; one charging is enough to be charging. No
     * "capacity": computed from energy_now/energy_full. */
    put("BAT1", "type", "Battery\n");
    put("BAT1", "energy_now", "20000000\n");
    put("BAT1", "energy_full", "50000000\n");
    put("BAT1", "status", "Charging\n");
    CHECK(kr_battery_read(dir, &b));
    CHECK_INT_EQ(b.count, 2);
    CHECK_INT_EQ(b.charge, 60); /* (80 + 40) / 2 */
    CHECK(b.charging && b.discharging);
    rm_rf();
}

TEST(sinks_from_pactl_json)
{
    const char *json =
        "[{\"index\":0,\"name\":\"alsa_output.a\",\"description\":\"Speakers\",\"mute\":false,"
        "\"volume\":{\"front-left\":{\"value\":16373},\"front-right\":{\"value\":20000}}},"
        "{\"index\":1,\"name\":\"bt\",\"description\":\"(null)\",\"mute\":true,"
        "\"volume\":{\"mono\":{\"value\":65536}},"
        "\"properties\":{\"device.description\":\"Headphones\"}},"
        "{\"index\":2,\"name\":\"plain\",\"description\":\"(null)\",\"properties\":{}},"
        "{\"index\":3,\"description\":\"no name\"}]";
    kr_sink s[8];
    int n = kr_sinks_parse(json, "bt", s, 8);
    CHECK_INT_EQ(n, 3);
    CHECK_STR_EQ(s[0].name, "alsa_output.a");
    CHECK_STR_EQ(s[0].description, "Speakers");
    CHECK_INT_EQ(s[0].volume, 20000); /* the loudest channel */
    CHECK(!s[0].muted && !s[0].enabled);
    CHECK_STR_EQ(s[1].description, "Headphones"); /* from the property */
    CHECK(s[1].muted && s[1].enabled);
    CHECK_STR_EQ(s[2].description, "plain");       /* last resort: the name */
    CHECK_INT_EQ(s[2].volume, 0);

    CHECK_INT_EQ(kr_sinks_parse("not json", NULL, s, 8), 0);
    CHECK_INT_EQ(kr_sinks_parse("{}", NULL, s, 8), 0);
    CHECK_INT_EQ(kr_sinks_parse(json, NULL, s, 1), 1); /* respects max */
}

TEST(ring_sound_is_a_wav)
{
    make_dir();
    char path[KR_PATH_MAX];
    kr_pathf(path, sizeof path, "%s/ring.wav", dir);
    CHECK(kr_find_write_wav(path));
    size_t len;
    char *wav = kr_read_file(path, &len);
    CHECK(wav);
    CHECK(memcmp(wav, "RIFF", 4) == 0 && memcmp(wav + 8, "WAVEfmt ", 8) == 0);
    CHECK(memcmp(wav + 36, "data", 4) == 0);
    unsigned data = (unsigned char)wav[40] | (unsigned char)wav[41] << 8 | (unsigned char)wav[42] << 16 |
                    (unsigned)(unsigned char)wav[43] << 24;
    CHECK_INT_EQ(len, 44 + data);
    CHECK(data > 22050 * 2 * 8); /* about 9 s of mono 16-bit */
    /* Not silence. */
    bool loud = false;
    for (size_t i = 44; i + 1 < len && !loud; i += 2)
        loud = (short)((unsigned char)wav[i] | (unsigned char)wav[i + 1] << 8) > 10000;
    CHECK(loud);
    free(wav);
    rm_rf();
}

TEST(plugins_can_be_switched_off)
{
    kr_config *c = kr_config_parse("");
    cJSON *in = kr_plugins_capabilities(c, true), *out = kr_plugins_capabilities(c, false);
    const char *want_in[] = {"kdeconnect.ping", "kdeconnect.battery", "kdeconnect.telephony",
                             "kdeconnect.mpris.request", "kdeconnect.systemvolume.request",
                             "kdeconnect.findmyphone.request", "kdeconnect.connectivity_report"};
    for (size_t i = 0; i < sizeof want_in / sizeof *want_in; i++) {
        bool found = false;
        const cJSON *e;
        cJSON_ArrayForEach(e, in) found |= strcmp(e->valuestring, want_in[i]) == 0;
        CHECK_MSG(found, "missing %s", want_in[i]);
    }
    /* The telephony type is taken by two plugins but listed once. */
    int telephony = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, in) telephony += strcmp(e->valuestring, "kdeconnect.telephony") == 0;
    CHECK_INT_EQ(telephony, 1);
    cJSON_Delete(in);
    cJSON_Delete(out);
    kr_config_free(c);

    c = kr_config_parse("[plugins]\nbattery = false\nping = no\nmpris = off\ntelephony = 0\n");
    CHECK(!kr_config_plugin_enabled(c, "battery"));
    CHECK(kr_config_plugin_enabled(c, "share"));
    in = kr_plugins_capabilities(c, true);
    cJSON_ArrayForEach(e, in)
    {
        CHECK(strcmp(e->valuestring, "kdeconnect.battery") != 0);
        CHECK(strcmp(e->valuestring, "kdeconnect.ping") != 0);
        CHECK(strcmp(e->valuestring, "kdeconnect.mpris.request") != 0);
    }
    cJSON_Delete(in);
    /* pausemusic also takes telephony, so it is still listed. */
    in = kr_plugins_capabilities(c, true);
    bool tel = false;
    cJSON_ArrayForEach(e, in) tel |= strcmp(e->valuestring, "kdeconnect.telephony") == 0;
    CHECK(tel);
    cJSON_Delete(in);
    kr_config_free(c);
}

TEST(booleans_android_style)
{
    /* Android sends isCancel as the string "true". */
    kr_packet *p = kr_packet_parse("{\"type\":\"kdeconnect.telephony\",\"body\":{\"a\":\"true\","
                                   "\"b\":\"false\",\"c\":true,\"d\":\"maybe\",\"e\":3}}", 100);
    CHECK(p);
    CHECK(kr_packet_bool(p, "a", false));
    CHECK(!kr_packet_bool(p, "b", true));
    CHECK(kr_packet_bool(p, "c", false));
    CHECK(kr_packet_bool(p, "d", true));   /* anything else: the fallback */
    CHECK(!kr_packet_bool(p, "d", false));
    CHECK(kr_packet_bool(p, "missing", true));
    CHECK(!kr_packet_bool(p, "e", false));
    kr_packet_free(p);
}

int main(void)
{
    RUN(battery_from_sysfs);
    RUN(sinks_from_pactl_json);
    RUN(ring_sound_is_a_wav);
    RUN(plugins_can_be_switched_off);
    RUN(booleans_android_style);
    return harness_report();
}
