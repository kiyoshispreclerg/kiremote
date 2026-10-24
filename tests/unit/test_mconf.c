/* SPDX-License-Identifier: GPL-3.0-or-later */
/* [mirror] in kiremote.ini, and every choice the settings offer for it. */
#include <string.h>

#include "../harness.h"
#include "conf.h"
#include "schema.h"

TEST(defaults_without_a_file)
{
    kr_mirror_conf c;
    kr_mirror_conf_load(&c, NULL);
    CHECK_INT_EQ(c.max_size, 0);
    CHECK_INT_EQ(c.bit_rate, 0);
    CHECK_STR_EQ(c.codec, "h264");
    CHECK(c.audio && c.control && c.clipboard && c.wifi);
    CHECK(!c.screen_off && !c.stay_awake);
}

TEST(reads_the_section)
{
    kr_config *cfg = kr_config_parse("[mirror]\nmax_size = 1280\nbit_rate = 4M\nmax_fps = 30\ncodec = h265\n"
                                     "audio = no\ncontrol = false\nclipboard = off\nscreen_off = yes\n"
                                     "stay_awake = 1\nwifi = no\n");
    kr_mirror_conf c;
    kr_mirror_conf_load(&c, cfg);
    CHECK_INT_EQ(c.max_size, 1280);
    CHECK_INT_EQ(c.bit_rate, 4000000);
    CHECK_INT_EQ(c.max_fps, 30);
    CHECK_STR_EQ(c.codec, "h265");
    CHECK(!c.audio && !c.control && !c.clipboard && !c.wifi);
    CHECK(c.screen_off && c.stay_awake);
    kr_config_free(cfg);
}

TEST(nonsense_falls_back)
{
    kr_config *cfg = kr_config_parse("[mirror]\nmax_size = big\nbit_rate = -4\nmax_fps = 0\ncodec = vp9\n");
    kr_mirror_conf c;
    kr_mirror_conf_load(&c, cfg);
    CHECK_INT_EQ(c.max_size, 0);
    CHECK_INT_EQ(c.bit_rate, 0);
    CHECK_INT_EQ(c.max_fps, 0);
    CHECK_STR_EQ(c.codec, "h264");
    kr_config_free(cfg);
}

TEST(bit_rates)
{
    unsigned v;
    CHECK(kr_parse_bit_rate("8M", &v) && v == 8000000);
    CHECK(kr_parse_bit_rate("500k", &v) && v == 500000);
    CHECK(kr_parse_bit_rate("123456", &v) && v == 123456);
    CHECK(!kr_parse_bit_rate("", &v));
    CHECK(!kr_parse_bit_rate("0", &v));
    CHECK(!kr_parse_bit_rate("8MB", &v));
    CHECK(!kr_parse_bit_rate("5000M", &v));
    CHECK(!kr_parse_bit_rate("M", &v));
}

/* The settings screen can only offer values the program reads. */
TEST(every_choice_is_read)
{
    int n;
    const kr_setting *all = kr_schema_all(&n);
    for (int i = 0; i < n; i++) {
        const kr_setting *s = &all[i];
        if (strcmp(s->section, "mirror") != 0 || s->type != KR_S_ENUM)
            continue;
        for (const char *const *ch = s->choices; *ch; ch++) {
            char ini[128];
            snprintf(ini, sizeof ini, "[mirror]\n%s = %s\n", s->key, *ch);
            kr_config *cfg = kr_config_parse(ini);
            kr_mirror_conf c;
            kr_mirror_conf_load(&c, cfg);
            if (!strcmp(s->key, "max_size"))
                CHECK_MSG(!**ch || c.max_size, "max_size %s", *ch);
            else if (!strcmp(s->key, "bit_rate"))
                CHECK_MSG(!**ch || c.bit_rate, "bit_rate %s", *ch);
            else if (!strcmp(s->key, "max_fps"))
                CHECK_MSG(!**ch || c.max_fps, "max_fps %s", *ch);
            else if (!strcmp(s->key, "codec"))
                CHECK_STR_EQ(c.codec, *ch);
            kr_config_free(cfg);
        }
    }
}

int main(void)
{
    RUN(defaults_without_a_file);
    RUN(reads_the_section);
    RUN(nonsense_falls_back);
    RUN(bit_rates);
    RUN(every_choice_is_read);
    return harness_report();
}
