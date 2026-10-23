/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_STREAM_H
#define KR_STREAM_H

/* The video or audio stream the scrcpy-server sends (docs/PROTOCOL.md,
 * "Espelhamento"), parsed as the bytes arrive, in whatever pieces: the
 * device name (first socket only), the codec, then sessions and packets.
 * No I/O and no libav, so the tests and the fuzzer drive it with bytes.
 *
 * H.264/H.265 config packets (SPS/PPS) are glued in front of the next
 * media packet, as the decoder wants them (scrcpy's sc_packet_merger);
 * other codecs get them on their own, with pts KR_STREAM_NO_PTS.
 *
 * The callbacks may not free the kr_stream. Packet data is only valid
 * during the call. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KR_STREAM_NAME_SIZE 64
#define KR_STREAM_NO_PTS INT64_C(-1)
/* Bigger than any encoded frame; more is garbage, not a frame. */
#define KR_STREAM_MAX_PACKET (32u << 20)
#define KR_STREAM_MAX_SIDE 16384u

/* The codec ids, as sent (4 ASCII bytes, big-endian). */
#define KR_CODEC_H264 UINT32_C(0x68323634)
#define KR_CODEC_H265 UINT32_C(0x68323635)
#define KR_CODEC_AV1 UINT32_C(0x00617631)
#define KR_CODEC_OPUS UINT32_C(0x6f707573)
#define KR_CODEC_AAC UINT32_C(0x00616163)
#define KR_CODEC_FLAC UINT32_C(0x666c6163)
#define KR_CODEC_RAW UINT32_C(0x00726177)

typedef struct {
    /* The device name, NUL-terminated (as sent: not checked for UTF-8). */
    void (*on_name)(void *ud, const char *name);
    /* A known codec of the stream's kind. Not called if the device turned
     * the stream off (kr_stream_disabled). */
    void (*on_codec)(void *ud, uint32_t codec);
    /* Video only: the size of what follows, before the first packet and
     * again whenever it changes (rotation). */
    void (*on_session)(void *ud, uint32_t width, uint32_t height, bool client_resized);
    void (*on_packet)(void *ud, const uint8_t *data, size_t len, int64_t pts, bool key_frame);
} kr_stream_cbs;

typedef struct kr_stream kr_stream;

/* with_name: this is the first socket (the 64-byte name comes first).
 * video: a video stream (sessions, video codecs), else audio. */
kr_stream *kr_stream_new(bool with_name, bool video, const kr_stream_cbs *cbs, void *ud);
void kr_stream_free(kr_stream *s);

/* false once the stream is broken (and from then on): kr_stream_error says
 * why. Bytes after the device turned the stream off are an error too. */
bool kr_stream_feed(kr_stream *s, const uint8_t *data, size_t len);
const char *kr_stream_error(const kr_stream *s);
/* The device sent codec 0: it could not capture this (audio before
 * Android 11, or no permission); the others go on. */
bool kr_stream_disabled(const kr_stream *s);

/* "h264" and so on; NULL for an unknown id. */
const char *kr_codec_name(uint32_t codec);

#endif
