/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Control and device messages of the scrcpy-server, against the bytes
 * upstream's own tests expect (upstream/scrcpy/app/tests/
 * test_control_msg_serialize.c, test_device_msg_deserialize.c). */
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "ctlmsg.h"

static uint8_t buf[KR_CTL_MAX_SIZE];

#define CHECK_BYTES(n, ...)                                                                        \
    do {                                                                                          \
        static const uint8_t want_[] = {__VA_ARGS__};                                             \
        CHECK_INT_EQ((n), sizeof want_);                                                          \
        CHECK(memcmp(buf, want_, sizeof want_) == 0);                                             \
    } while (0)

TEST(keycode)
{
    /* AKEYCODE_ENTER up, repeat 5, AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON */
    size_t n = kr_ctl_keycode(buf, KR_AKEY_UP, 66, 5, 0x41);
    CHECK_BYTES(n, 0, 0x01, 0, 0, 0, 0x42, 0, 0, 0, 0x05, 0, 0, 0, 0x41);
}

TEST(text_and_its_limit)
{
    size_t n = kr_ctl_text(buf, "hello, world!");
    CHECK_BYTES(n, 1, 0, 0, 0, 0x0d, 'h', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!');

    /* Cut at 300 bytes, never inside a character: 299 'a' then "é" (2
     * bytes) leaves the "é" out. */
    char s[400];
    memset(s, 'a', 299);
    memcpy(s + 299, "\xc3\xa9xyz", 6);
    n = kr_ctl_text(buf, s);
    CHECK_INT_EQ(n, 1 + 4 + 299);
    CHECK(buf[3] == 0x01 && buf[4] == 0x2b); /* 299 */
}

TEST(touch)
{
    size_t n = kr_ctl_touch(buf, KR_AMOTION_DOWN, UINT64_C(0x1234567887654321), 100, 200, 1080, 1920, 1.0f,
                            KR_ABUTTON_PRIMARY, KR_ABUTTON_PRIMARY);
    CHECK_BYTES(n, 2, 0x00, 0x12, 0x34, 0x56, 0x78, 0x87, 0x65, 0x43, 0x21, 0, 0, 0, 0x64, 0, 0, 0, 0xc8, 0x04, 0x38,
                0x07, 0x80, 0xff, 0xff, 0, 0, 0, 0x01, 0, 0, 0, 0x01);
    /* The mouse pointer is -1. */
    kr_ctl_touch(buf, KR_AMOTION_UP, KR_POINTER_MOUSE, 0, 0, 1, 1, 0, 0, 0);
    for (int i = 2; i < 10; i++)
        CHECK(buf[i] == 0xff);
    CHECK(buf[22] == 0 && buf[23] == 0); /* pressure 0 */
}

TEST(scroll)
{
    size_t n = kr_ctl_scroll(buf, 260, 1026, 1080, 1920, 16, -16, 1);
    CHECK_BYTES(n, 3, 0, 0, 0x01, 0x04, 0, 0, 0x04, 0x02, 0x04, 0x38, 0x07, 0x80, 0x7f, 0xff, 0x80, 0x00, 0, 0, 0,
                0x01);
    /* Beyond the range: clamped. */
    kr_ctl_scroll(buf, 0, 0, 1, 1, 100, -100, 0);
    CHECK(buf[13] == 0x7f && buf[14] == 0xff && buf[15] == 0x80 && buf[16] == 0);
}

TEST(short_ones)
{
    CHECK_BYTES(kr_ctl_back_or_screen_on(buf, KR_AKEY_UP), 4, 0x01);
    CHECK_BYTES(kr_ctl_simple(buf, KR_CTL_EXPAND_NOTIFICATIONS), 5);
    CHECK_BYTES(kr_ctl_simple(buf, KR_CTL_ROTATE_DEVICE), 11);
    CHECK_BYTES(kr_ctl_set_display_power(buf, true), 10, 0x01);
}

TEST(set_clipboard)
{
    size_t n = kr_ctl_set_clipboard(buf, UINT64_C(0x0102030405060708), "hello, world!", true);
    CHECK_BYTES(n, 9, 1, 2, 3, 4, 5, 6, 7, 8, 1, 0, 0, 0, 0x0d, 'h', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l',
                'd', '!');
}

typedef struct {
    char text[64];
    int clips;
    uint64_t ack;
} seen;

static void on_clip(void *ud, const char *text, size_t len)
{
    seen *s = ud;
    if (len < sizeof s->text)
        memcpy(s->text, text, len + 1);
    s->clips++;
}

static void on_ack(void *ud, uint64_t seq)
{
    ((seen *)ud)->ack = seq;
}

TEST(device_messages_in_any_pieces)
{
    static const uint8_t in[] = {0, 0, 0, 0, 3, 'A', 'B', 'C',          /* clipboard "ABC" */
                                 1, 1, 2, 3, 4, 5, 6, 7, 8,             /* ack */
                                 2, 0, 7, 0, 2, 0xaa, 0xbb,             /* uhid output: skipped */
                                 0, 0, 0, 0, 0};                        /* empty clipboard */
    seen s = {0};
    static const kr_devmsg_cbs cbs = {on_clip, on_ack};
    kr_devmsg *p = kr_devmsg_new(&cbs, &s);
    for (size_t i = 0; i < sizeof in; i++)
        CHECK(kr_devmsg_feed(p, in + i, 1));
    CHECK_INT_EQ(s.clips, 2);
    CHECK_STR_EQ(s.text, "");
    CHECK(s.ack == UINT64_C(0x0102030405060708));
    kr_devmsg_free(p);

    s = (seen){0};
    p = kr_devmsg_new(&cbs, &s);
    CHECK(kr_devmsg_feed(p, in, 8));
    CHECK_STR_EQ(s.text, "ABC");
    kr_devmsg_free(p);
}

TEST(a_broken_device_stream_stays_broken)
{
    seen s = {0};
    static const kr_devmsg_cbs cbs = {on_clip, on_ack};
    kr_devmsg *p = kr_devmsg_new(&cbs, &s);
    static const uint8_t bad[] = {9, 0, 0};
    CHECK(!kr_devmsg_feed(p, bad, sizeof bad));
    static const uint8_t ok[] = {1, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(!kr_devmsg_feed(p, ok, sizeof ok));
    CHECK(!s.ack);
    kr_devmsg_free(p);

    /* A clipboard bigger than the server ever sends. */
    p = kr_devmsg_new(&cbs, &s);
    static const uint8_t huge[] = {0, 0x7f, 0, 0, 0};
    CHECK(!kr_devmsg_feed(p, huge, sizeof huge));
    kr_devmsg_free(p);
}

int main(void)
{
    RUN(keycode);
    RUN(text_and_its_limit);
    RUN(touch);
    RUN(scroll);
    RUN(short_ones);
    RUN(set_clipboard);
    RUN(device_messages_in_any_pieces);
    RUN(a_broken_device_stream_stays_broken);
    return harness_report();
}
