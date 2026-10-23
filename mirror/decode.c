/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libavcodec behind kr_decoder (decode.h); as upstream/scrcpy's
 * app/src/decoder.c, without its threads and queues. */
#include "decode.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "stream.h"

struct kr_decoder {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
};

kr_decoder *kr_decoder_new(uint32_t codec)
{
    enum AVCodecID id = codec == KR_CODEC_H264   ? AV_CODEC_ID_H264
                        : codec == KR_CODEC_H265 ? AV_CODEC_ID_HEVC
                        : codec == KR_CODEC_AV1  ? AV_CODEC_ID_AV1
                                                 : AV_CODEC_ID_NONE;
    const AVCodec *c = id == AV_CODEC_ID_NONE ? NULL : avcodec_find_decoder(id);
    if (!c)
        return NULL;
    kr_decoder *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->ctx = avcodec_alloc_context3(c);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->pkt || !d->frame)
        goto fail;
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->ctx->thread_type = FF_THREAD_SLICE; /* frame threads add a frame of delay each */
    d->ctx->thread_count = 0;              /* as many as there are cores */
    if (avcodec_open2(d->ctx, c, NULL) < 0)
        goto fail;
    return d;
fail:
    kr_decoder_free(d);
    return NULL;
}

void kr_decoder_free(kr_decoder *d)
{
    if (!d)
        return;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->ctx);
    free(d);
}

bool kr_decoder_push(kr_decoder *d, const uint8_t *data, size_t len, int64_t pts,
                     void (*on_frame)(void *ud, const AVFrame *frame), void *ud)
{
    if (len > INT32_MAX || av_new_packet(d->pkt, (int)len) < 0)
        return false;
    memcpy(d->pkt->data, data, len);
    d->pkt->pts = d->pkt->dts = pts == KR_STREAM_NO_PTS ? AV_NOPTS_VALUE : pts;
    int r = avcodec_send_packet(d->ctx, d->pkt);
    av_packet_unref(d->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        /* A bad packet: the next key frame repairs it. */
        kr_log_debug("decoder: packet refused (%d)", r);
        return r != AVERROR(ENOMEM);
    }
    for (;;) {
        r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return true;
        if (r < 0) {
            kr_log_debug("decoder: no frame (%d)", r);
            return r != AVERROR(ENOMEM);
        }
        on_frame(ud, d->frame);
        av_frame_unref(d->frame);
    }
}
