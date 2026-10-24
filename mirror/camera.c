/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "camera.h"

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

struct kr_camera {
    kr_v4l2 *out;
    unsigned w, h;
    bool flip;
    uint8_t *buf; /* Y, then U, then V, as V4L2's YU12 */
    struct SwsContext *sws;
};

kr_camera *kr_camera_new(kr_v4l2 *out, unsigned width, unsigned height, bool flip)
{
    kr_camera *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->out = out;
    c->w = width;
    c->h = height;
    c->flip = flip;
    c->buf = malloc(kr_v4l2_frame_size(out));
    if (!c->buf) {
        free(c);
        return NULL;
    }
    memset(c->buf, 128, kr_v4l2_frame_size(out));
    return c;
}

void kr_camera_free(kr_camera *c)
{
    if (!c)
        return;
    kr_v4l2_close(c->out);
    sws_freeContext(c->sws);
    free(c->buf);
    free(c);
}

static void flip_plane(uint8_t *p, unsigned w, unsigned h)
{
    for (unsigned y = 0; y < h; y++) {
        uint8_t *row = p + (size_t)y * w;
        for (unsigned a = 0, b = w - 1; a < b; a++, b--) {
            uint8_t t = row[a];
            row[a] = row[b];
            row[b] = t;
        }
    }
}

bool kr_camera_frame(kr_camera *c, const AVFrame *f)
{
    if (f->width <= 0 || f->height <= 0)
        return true;
    c->sws = sws_getCachedContext(c->sws, f->width, f->height, (enum AVPixelFormat)f->format, (int)c->w, (int)c->h,
                                  AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!c->sws)
        return true;
    uint8_t *dst[4] = {c->buf, c->buf + (size_t)c->w * c->h, c->buf + (size_t)c->w * c->h * 5 / 4, NULL};
    int stride[4] = {(int)c->w, (int)c->w / 2, (int)c->w / 2, 0};
    sws_scale(c->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride);
    if (c->flip) {
        flip_plane(dst[0], c->w, c->h);
        flip_plane(dst[1], c->w / 2, c->h / 2);
        flip_plane(dst[2], c->w / 2, c->h / 2);
    }
    return kr_v4l2_write(c->out, c->buf);
}
