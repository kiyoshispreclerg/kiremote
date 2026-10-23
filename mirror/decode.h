/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_DECODE_H
#define KR_DECODE_H

/* The video decoder: packets from kr_stream in, frames out (libavcodec, in
 * software, slice threads only so a frame is not held back). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AVFrame;

typedef struct kr_decoder kr_decoder;

/* codec: KR_CODEC_H264, _H265 or _AV1. NULL if libavcodec has no decoder
 * for it. */
kr_decoder *kr_decoder_new(uint32_t codec);
void kr_decoder_free(kr_decoder *d);

/* A packet (already merged with its config). on_frame gets each frame it
 * completes (owned by the decoder, valid during the call). false on a
 * decoding error the stream cannot go on from; a damaged frame is only
 * skipped. */
bool kr_decoder_push(kr_decoder *d, const uint8_t *data, size_t len, int64_t pts,
                     void (*on_frame)(void *ud, const struct AVFrame *frame), void *ud);

#endif
