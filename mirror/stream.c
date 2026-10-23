/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The scrcpy-server's video and audio streams, as a state machine over the
 * bytes (stream.h). Read in upstream/scrcpy: app/src/demuxer.c (the
 * client), server/.../device/Streamer.java and DesktopConnection.java. */
#include "stream.h"

#include <stdlib.h>
#include <string.h>

#include "i18n.h"

#define HEADER_SIZE 12
#define FLAG_SESSION (UINT64_C(1) << 63)
#define FLAG_CONFIG (UINT64_C(1) << 62)
#define FLAG_KEY_FRAME (UINT64_C(1) << 61)
#define PTS_MASK (FLAG_KEY_FRAME - 1)

typedef enum { ST_NAME, ST_CODEC, ST_HEADER, ST_BODY, ST_DISABLED, ST_FAILED } state;

struct kr_stream {
    kr_stream_cbs cbs;
    void *ud;
    bool video;
    state st;
    uint32_t codec;
    bool merge;        /* H.264/H.265: config goes in front of the next packet */
    bool had_session;  /* video: the first header must be a session */
    uint8_t small[KR_STREAM_NAME_SIZE]; /* name, codec or header being filled */
    size_t have, need;
    /* The packet being read; with merge, after the pending config. */
    uint8_t *body;
    size_t body_cap, body_off, body_len;
    uint64_t pts_flags;
    size_t config_len; /* pending config at the start of body */
    const char *error;
};

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

const char *kr_codec_name(uint32_t codec)
{
    switch (codec) {
    case KR_CODEC_H264: return "h264";
    case KR_CODEC_H265: return "h265";
    case KR_CODEC_AV1: return "av1";
    case KR_CODEC_OPUS: return "opus";
    case KR_CODEC_AAC: return "aac";
    case KR_CODEC_FLAC: return "flac";
    case KR_CODEC_RAW: return "raw";
    default: return NULL;
    }
}

static bool is_video_codec(uint32_t c)
{
    return c == KR_CODEC_H264 || c == KR_CODEC_H265 || c == KR_CODEC_AV1;
}

kr_stream *kr_stream_new(bool with_name, bool video, const kr_stream_cbs *cbs, void *ud)
{
    kr_stream *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->cbs = *cbs;
    s->ud = ud;
    s->video = video;
    s->st = with_name ? ST_NAME : ST_CODEC;
    s->need = with_name ? KR_STREAM_NAME_SIZE : 4;
    return s;
}

void kr_stream_free(kr_stream *s)
{
    if (!s)
        return;
    free(s->body);
    free(s);
}

const char *kr_stream_error(const kr_stream *s)
{
    return s->error;
}

bool kr_stream_disabled(const kr_stream *s)
{
    return s->st == ST_DISABLED;
}

static bool fail(kr_stream *s, const char *why)
{
    s->st = ST_FAILED;
    s->error = why;
    return false;
}

static void expect_header(kr_stream *s)
{
    s->st = ST_HEADER;
    s->have = 0;
    s->need = HEADER_SIZE;
}

/* A whole name, codec or header is in s->small. */
static bool small_done(kr_stream *s)
{
    if (s->st == ST_NAME) {
        s->small[KR_STREAM_NAME_SIZE - 1] = 0; /* as the client does */
        if (s->cbs.on_name)
            s->cbs.on_name(s->ud, (const char *)s->small);
        s->st = ST_CODEC;
        s->have = 0;
        s->need = 4;
        return true;
    }
    if (s->st == ST_CODEC) {
        uint32_t c = be32(s->small);
        if (c == 0) {
            s->st = ST_DISABLED;
            return true;
        }
        if (c == 1)
            return fail(s, _("The phone could not set up the stream (configuration error)"));
        if (!kr_codec_name(c) || is_video_codec(c) != s->video)
            return fail(s, _("The phone sent an unknown codec"));
        s->codec = c;
        s->merge = c == KR_CODEC_H264 || c == KR_CODEC_H265;
        if (s->cbs.on_codec)
            s->cbs.on_codec(s->ud, c);
        expect_header(s);
        return true;
    }
    /* A 12-byte header. */
    uint64_t pf = be64(s->small);
    if (pf & FLAG_SESSION) {
        if (!s->video)
            return fail(s, _("Unexpected session in an audio stream"));
        uint32_t w = be32(s->small + 4), h = be32(s->small + 8);
        if (!w || !h || w > KR_STREAM_MAX_SIDE || h > KR_STREAM_MAX_SIDE)
            return fail(s, _("The phone sent an invalid video size"));
        s->had_session = true;
        if (s->cbs.on_session)
            s->cbs.on_session(s->ud, w, h, s->small[3] & 1);
        expect_header(s);
        return true;
    }
    if (s->video && !s->had_session)
        return fail(s, _("The video stream did not start with its size"));
    uint32_t len = be32(s->small + 8);
    if (!len || len > KR_STREAM_MAX_PACKET)
        return fail(s, _("The phone sent a packet of invalid size"));
    size_t total = s->config_len + len;
    if (total > s->body_cap) {
        uint8_t *b = realloc(s->body, total);
        if (!b)
            return fail(s, _("Out of memory"));
        s->body = b;
        s->body_cap = total;
    }
    s->pts_flags = pf;
    s->body_off = s->config_len;
    s->body_len = len;
    s->st = ST_BODY;
    return true;
}

/* The packet in s->body is complete. */
static void body_done(kr_stream *s)
{
    bool config = s->pts_flags & FLAG_CONFIG;
    if (config && s->merge) {
        /* Kept, alone at the start of the buffer, for the next packet (a
         * newer config replaces it). */
        if (s->body_off)
            memmove(s->body, s->body + s->body_off, s->body_len);
        s->config_len = s->body_len;
    } else {
        int64_t pts = config ? KR_STREAM_NO_PTS : (int64_t)(s->pts_flags & PTS_MASK);
        size_t len = s->body_off + s->body_len; /* with the pending config, if any */
        s->config_len = 0;
        if (s->cbs.on_packet)
            s->cbs.on_packet(s->ud, s->body, len, pts, (s->pts_flags & FLAG_KEY_FRAME) != 0);
    }
    expect_header(s);
}

bool kr_stream_feed(kr_stream *s, const uint8_t *data, size_t len)
{
    while (len) {
        if (s->st == ST_FAILED)
            return false;
        if (s->st == ST_DISABLED)
            return fail(s, _("The phone sent data on a stream it turned off"));
        if (s->st == ST_BODY) {
            size_t left = s->body_len - s->have;
            size_t n = len < left ? len : left;
            memcpy(s->body + s->body_off + s->have, data, n);
            s->have += n;
            data += n;
            len -= n;
            if (s->have == s->body_len) {
                s->have = 0;
                body_done(s);
            }
            continue;
        }
        size_t n = s->need - s->have;
        if (n > len)
            n = len;
        memcpy(s->small + s->have, data, n);
        s->have += n;
        data += n;
        len -= n;
        if (s->have == s->need) {
            s->have = 0;
            if (!small_done(s))
                return false;
        }
    }
    return s->st != ST_FAILED;
}
