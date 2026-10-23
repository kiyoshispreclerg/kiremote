/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The scrcpy-server streams: a capture from a real phone (Android 12,
 * h264, max_size=800: name, codec, session, config, a key frame and three
 * frames), fed whole and byte by byte, and the ways a stream can be wrong. */
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "stream.h"
#include "util.h"

typedef struct {
    char name[KR_STREAM_NAME_SIZE];
    uint32_t codec;
    int sessions;
    uint32_t w, h;
    int packets;
    size_t first_len, total;
    int64_t first_pts, last_pts;
    bool first_key;
    int no_pts;
} seen;

static void on_name(void *ud, const char *name)
{
    seen *s = ud;
    snprintf(s->name, sizeof s->name, "%s", name);
}

static void on_codec(void *ud, uint32_t codec)
{
    ((seen *)ud)->codec = codec;
}

static void on_session(void *ud, uint32_t w, uint32_t h, bool resized)
{
    seen *s = ud;
    (void)resized;
    s->sessions++;
    s->w = w;
    s->h = h;
}

static void on_packet(void *ud, const uint8_t *data, size_t len, int64_t pts, bool key)
{
    seen *s = ud;
    (void)data;
    if (!s->packets) {
        s->first_len = len;
        s->first_pts = pts;
        s->first_key = key;
    }
    if (pts == KR_STREAM_NO_PTS)
        s->no_pts++;
    s->last_pts = pts;
    s->packets++;
    s->total += len;
}

static const kr_stream_cbs cbs = {on_name, on_codec, on_session, on_packet};

static uint8_t *fixture(size_t *len)
{
    return (uint8_t *)kr_read_file(KR_FIXTURES_DIR "/scrcpy-video.bin", len);
}

static void check_capture(const seen *s)
{
    if (strcmp(s->name, "2201117TL") || s->codec != KR_CODEC_H264 || s->sessions != 1 || s->w != 360 ||
        s->h != 800 || s->packets != 4 || !s->first_key || s->first_pts != INT64_C(201277631330) ||
        s->first_len != 31 + 23648 || s->total != 31 + 23648 + 816 + 240 + 208 || s->no_pts)
        harness_current_failed = 1;
}

TEST(a_capture_from_the_phone_parses_whole)
{
    size_t len;
    uint8_t *d = fixture(&len);
    CHECK(d);
    seen s = {0};
    kr_stream *st = kr_stream_new(true, true, &cbs, &s);
    CHECK(kr_stream_feed(st, d, len));
    check_capture(&s);
    CHECK_MSG(!harness_current_failed, "name %s codec %08x %ux%u packets %d first %zu", s.name, s.codec, s.w, s.h,
              s.packets, s.first_len);
    CHECK(!kr_stream_error(st));
    kr_stream_free(st);
    free(d);
}

TEST(a_capture_from_the_phone_parses_byte_by_byte)
{
    size_t len;
    uint8_t *d = fixture(&len);
    CHECK(d);
    seen s = {0};
    kr_stream *st = kr_stream_new(true, true, &cbs, &s);
    for (size_t i = 0; i < len; i++)
        CHECK(kr_stream_feed(st, d + i, 1));
    check_capture(&s);
    CHECK(!harness_current_failed);
    kr_stream_free(st);
    free(d);
}

static size_t put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return 4;
}

static size_t header(uint8_t *p, uint64_t pf, uint32_t len)
{
    put32(p, (uint32_t)(pf >> 32));
    put32(p + 4, (uint32_t)pf);
    put32(p + 8, len);
    return 12;
}

static bool feed_one(bool video, const uint8_t *d, size_t len, seen *s, kr_stream **out)
{
    kr_stream *st = kr_stream_new(false, video, &cbs, s);
    bool ok = kr_stream_feed(st, d, len);
    if (out)
        *out = st;
    else
        kr_stream_free(st);
    return ok;
}

TEST(codec_zero_turns_the_stream_off)
{
    uint8_t d[8] = {0};
    seen s = {0};
    kr_stream *st;
    CHECK(feed_one(false, d, 4, &s, &st));
    CHECK(kr_stream_disabled(st));
    CHECK(!s.codec);
    /* Nothing may follow. */
    CHECK(!kr_stream_feed(st, d, 1));
    CHECK(kr_stream_error(st));
    kr_stream_free(st);
}

TEST(codec_one_is_a_configuration_error)
{
    uint8_t d[4] = {0, 0, 0, 1};
    seen s = {0};
    kr_stream *st;
    CHECK(!feed_one(true, d, 4, &s, &st));
    CHECK(kr_stream_error(st));
    CHECK(!kr_stream_disabled(st));
    kr_stream_free(st);
}

TEST(codecs_must_match_the_kind_of_stream)
{
    uint8_t d[4];
    seen s = {0};
    put32(d, KR_CODEC_OPUS);
    CHECK(!feed_one(true, d, 4, &s, NULL));
    put32(d, KR_CODEC_H264);
    CHECK(!feed_one(false, d, 4, &s, NULL));
    put32(d, 0x12345678);
    CHECK(!feed_one(true, d, 4, &s, NULL));
    CHECK(!s.codec);
}

TEST(video_starts_with_a_valid_session)
{
    uint8_t d[64];
    size_t n = put32(d, KR_CODEC_H264);
    n += header(d + n, 0, 3); /* a packet before any size */
    seen s = {0};
    CHECK(!feed_one(true, d, n, &s, NULL));

    n = put32(d, KR_CODEC_H264);
    n += header(d + n, (UINT64_C(1) << 63) | 0, 0); /* 0x0 */
    CHECK(!feed_one(true, d, n, &s, NULL));

    n = put32(d, KR_CODEC_H264);
    n += header(d + n, (UINT64_C(1) << 63) | ((uint64_t)(KR_STREAM_MAX_SIDE + 1) << 0), 100);
    CHECK(!feed_one(true, d, n, &s, NULL));
}

TEST(audio_has_no_sessions)
{
    uint8_t d[32];
    size_t n = put32(d, KR_CODEC_RAW);
    n += header(d + n, (UINT64_C(1) << 63) | 640, 480);
    seen s = {0};
    CHECK(!feed_one(false, d, n, &s, NULL));
}

TEST(packet_sizes_are_checked)
{
    uint8_t d[32];
    size_t n = put32(d, KR_CODEC_RAW);
    n += header(d + n, 5, 0);
    seen s = {0};
    CHECK(!feed_one(false, d, n, &s, NULL));
    n = put32(d, KR_CODEC_RAW);
    n += header(d + n, 5, KR_STREAM_MAX_PACKET + 1);
    CHECK(!feed_one(false, d, n, &s, NULL));
}

TEST(config_is_merged_only_for_h26x)
{
    /* Opus: the config packet comes on its own, without pts. */
    uint8_t d[64];
    size_t n = put32(d, KR_CODEC_OPUS);
    n += header(d + n, UINT64_C(1) << 62, 3);
    memcpy(d + n, "cfg", 3);
    n += 3;
    n += header(d + n, 1000, 2);
    memcpy(d + n, "ab", 2);
    n += 2;
    seen s = {0};
    CHECK(feed_one(false, d, n, &s, NULL));
    CHECK_INT_EQ(s.packets, 2);
    CHECK_INT_EQ(s.no_pts, 1);
    CHECK_INT_EQ(s.total, 5);
    CHECK_INT_EQ(s.last_pts, 1000);

    /* H.265: two configs in a row, the newer one wins. */
    n = put32(d, KR_CODEC_H265);
    n += header(d + n, (UINT64_C(1) << 63) | 640, 480);
    n += header(d + n, UINT64_C(1) << 62, 4);
    memcpy(d + n, "old!", 4);
    n += 4;
    n += header(d + n, UINT64_C(1) << 62, 2);
    memcpy(d + n, "NW", 2);
    n += 2;
    n += header(d + n, (UINT64_C(1) << 61) | 7, 3);
    memcpy(d + n, "key", 3);
    n += 3;
    s = (seen){0};
    CHECK(feed_one(true, d, n, &s, NULL));
    CHECK_INT_EQ(s.packets, 1);
    CHECK_INT_EQ(s.first_len, 5);
    CHECK_INT_EQ(s.first_pts, 7);
    CHECK(s.first_key);
}

int main(void)
{
    RUN(a_capture_from_the_phone_parses_whole);
    RUN(a_capture_from_the_phone_parses_byte_by_byte);
    RUN(codec_zero_turns_the_stream_off);
    RUN(codec_one_is_a_configuration_error);
    RUN(codecs_must_match_the_kind_of_stream);
    RUN(video_starts_with_a_valid_session);
    RUN(audio_has_no_sessions);
    RUN(packet_sizes_are_checked);
    RUN(config_is_merged_only_for_h26x);
    return harness_report();
}
