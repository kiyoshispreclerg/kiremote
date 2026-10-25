/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pointer_img.h"

#include <ctype.h>
#include <gif_lib.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "i18n.h"

static bool fail(char *err, size_t size, const char *msg)
{
    if (err && size)
        snprintf(err, size, "%s", msg);
    return false;
}

void kr_img_free(kr_img *img)
{
    for (int i = 0; img->px && i < img->n; i++)
        free(img->px[i]);
    free(img->px);
    free(img->delay_ms);
    memset(img, 0, sizeof *img);
}

bool kr_color_parse(const char *text, uint32_t *rgb)
{
    if (!text || text[0] != '#' || strlen(text) != 7)
        return false;
    uint32_t v = 0;
    for (int i = 1; i < 7; i++) {
        if (!isxdigit((unsigned char)text[i]))
            return false;
        int c = tolower((unsigned char)text[i]);
        v = v << 4 | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    *rgb = v;
    return true;
}

/* ---- scaling ------------------------------------------------------------- */

/* Each destination pixel is the average (premultiplied, so a transparent
 * pixel does not tint its neighbours) of the source pixels it covers; when
 * enlarging that is one pixel, i.e. the nearest. Returns a new array. */
static uint32_t *scale(const uint32_t *src, int sw, int sh, int dw, int dh)
{
    uint32_t *dst = malloc((size_t)dw * (size_t)dh * sizeof *dst);
    if (!dst)
        return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)(((int64_t)(y + 1) * sh + dh - 1) / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)(((int64_t)(x + 1) * sw + dw - 1) / dw);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint64_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1 && sy < sh; sy++)
                for (int sx = x0; sx < x1 && sx < sw; sx++) {
                    uint32_t p = src[(size_t)sy * (size_t)sw + (size_t)sx];
                    uint64_t pa = p >> 24;
                    a += pa;
                    r += pa * ((p >> 16) & 0xff);
                    g += pa * ((p >> 8) & 0xff);
                    b += pa * (p & 0xff);
                    n++;
                }
            uint32_t out = 0;
            if (n && a) {
                out = (uint32_t)(a / n) << 24 | (uint32_t)(r / a) << 16 | (uint32_t)(g / a) << 8 | (uint32_t)(b / a);
            }
            dst[(size_t)y * (size_t)dw + (size_t)x] = out;
        }
    }
    return dst;
}

/* Replaces every frame of img (all sw x sh) by one that fits in side x side. */
static bool fit(kr_img *img, int side)
{
    int sw = img->w, sh = img->h;
    double k = (double)side / (sw > sh ? sw : sh);
    int dw = (int)lround(sw * k), dh = (int)lround(sh * k);
    dw = dw < 1 ? 1 : dw;
    dh = dh < 1 ? 1 : dh;
    if (dw == sw && dh == sh)
        return true;
    for (int i = 0; i < img->n; i++) {
        uint32_t *s = scale(img->px[i], sw, sh, dw, dh);
        if (!s)
            return false;
        free(img->px[i]);
        img->px[i] = s;
    }
    img->w = dw;
    img->h = dh;
    return true;
}

/* ---- PNG ----------------------------------------------------------------- */

static bool load_png(const char *path, kr_img *img, char *err, size_t err_size)
{
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path))
        return fail(err, err_size, _("not a valid PNG image"));
    if (im.width == 0 || im.height == 0 || im.width > KR_IMG_MAX_SIDE || im.height > KR_IMG_MAX_SIDE) {
        png_image_free(&im);
        return fail(err, err_size, _("the image is too big"));
    }
    im.format = PNG_FORMAT_RGBA;
    size_t n = (size_t)im.width * im.height;
    uint8_t *raw = malloc(PNG_IMAGE_SIZE(im));
    uint32_t *px = malloc(n * sizeof *px);
    if (!raw || !px || !png_image_finish_read(&im, NULL, raw, 0, NULL)) {
        free(raw);
        free(px);
        png_image_free(&im);
        return fail(err, err_size, _("not a valid PNG image"));
    }
    for (size_t i = 0; i < n; i++)
        px[i] = (uint32_t)raw[i * 4 + 3] << 24 | (uint32_t)raw[i * 4] << 16 | (uint32_t)raw[i * 4 + 1] << 8 | raw[i * 4 + 2];
    free(raw);
    png_image_free(&im);
    img->px = calloc(1, sizeof *img->px);
    img->delay_ms = calloc(1, sizeof *img->delay_ms);
    if (!img->px || !img->delay_ms) {
        free(px);
        kr_img_free(img);
        return false;
    }
    img->px[0] = px;
    img->n = 1;
    img->w = (int)im.width;
    img->h = (int)im.height;
    return true;
}

/* ---- GIF ----------------------------------------------------------------- */

static void gif_close(GifFileType *g)
{
#if GIFLIB_MAJOR > 5 || (GIFLIB_MAJOR == 5 && GIFLIB_MINOR >= 1)
    int e;
    DGifCloseFile(g, &e);
#else
    DGifCloseFile(g);
#endif
}

static bool load_gif(const char *path, kr_img *img, char *err, size_t err_size)
{
    int e = 0;
    GifFileType *g = DGifOpenFileName(path, &e);
    if (!g)
        return fail(err, err_size, _("not a valid GIF image"));
    if (g->SWidth <= 0 || g->SHeight <= 0 || g->SWidth > KR_IMG_MAX_SIDE || g->SHeight > KR_IMG_MAX_SIDE) {
        gif_close(g);
        return fail(err, err_size, _("the image is too big"));
    }
    if (DGifSlurp(g) != GIF_OK || g->ImageCount < 1) {
        gif_close(g);
        return fail(err, err_size, _("not a valid GIF image"));
    }
    int w = g->SWidth, h = g->SHeight;
    size_t area = (size_t)w * (size_t)h;
    int count = g->ImageCount > KR_IMG_MAX_FRAMES ? KR_IMG_MAX_FRAMES : g->ImageCount;
    if (area * (size_t)count > KR_IMG_MAX_PIXELS) {
        gif_close(g);
        return fail(err, err_size, _("the image is too big"));
    }

    uint32_t *canvas = calloc(area, sizeof *canvas), *saved = malloc(area * sizeof *saved);
    img->px = calloc((size_t)count, sizeof *img->px);
    img->delay_ms = calloc((size_t)count, sizeof *img->delay_ms);
    bool ok = canvas && saved && img->px && img->delay_ms;

    for (int i = 0; ok && i < count; i++) {
        const SavedImage *si = &g->SavedImages[i];
        GraphicsControlBlock gcb = {.DisposalMode = DISPOSAL_UNSPECIFIED, .DelayTime = 0, .TransparentColor = NO_TRANSPARENT_COLOR};
        DGifSavedExtensionToGCB(g, i, &gcb);
        const ColorMapObject *map = si->ImageDesc.ColorMap ? si->ImageDesc.ColorMap : g->SColorMap;
        if (gcb.DisposalMode == DISPOSE_PREVIOUS)
            memcpy(saved, canvas, area * sizeof *canvas);

        const GifImageDesc *d = &si->ImageDesc;
        for (int y = 0; map && si->RasterBits && y < d->Height; y++) {
            int cy = d->Top + y;
            if (cy < 0 || cy >= h)
                continue;
            for (int x = 0; x < d->Width; x++) {
                int cx = d->Left + x;
                if (cx < 0 || cx >= w)
                    continue;
                int idx = si->RasterBits[(size_t)y * (size_t)d->Width + (size_t)x];
                if (idx == gcb.TransparentColor || idx >= map->ColorCount)
                    continue;
                const GifColorType *c = &map->Colors[idx];
                canvas[(size_t)cy * (size_t)w + (size_t)cx] = 0xff000000u | (uint32_t)c->Red << 16 | (uint32_t)c->Green << 8 | c->Blue;
            }
        }
        img->px[i] = malloc(area * sizeof **img->px);
        if (!img->px[i]) {
            ok = false;
            break;
        }
        memcpy(img->px[i], canvas, area * sizeof *canvas);
        img->n = i + 1;
        int ms = gcb.DelayTime * 10;
        img->delay_ms[i] = ms < 20 ? 100 : ms; /* what browsers do with 0 and 1 */

        if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
            for (int y = 0; y < d->Height; y++)
                for (int x = 0; x < d->Width; x++) {
                    int cx = d->Left + x, cy = d->Top + y;
                    if (cx >= 0 && cx < w && cy >= 0 && cy < h)
                        canvas[(size_t)cy * (size_t)w + (size_t)cx] = 0;
                }
        } else if (gcb.DisposalMode == DISPOSE_PREVIOUS) {
            memcpy(canvas, saved, area * sizeof *canvas);
        }
    }
    free(canvas);
    free(saved);
    gif_close(g);
    if (!ok) {
        kr_img_free(img);
        return false;
    }
    img->w = w;
    img->h = h;
    return true;
}

bool kr_img_load(const char *path, int side, kr_img *img, char *err, size_t err_size)
{
    memset(img, 0, sizeof *img);
    struct stat st;
    if (side < 1 || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return fail(err, err_size, _("cannot read the image"));
    if (st.st_size > (off_t)KR_IMG_MAX_FILE)
        return fail(err, err_size, _("the image file is too big"));
    unsigned char magic[4] = {0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail(err, err_size, _("cannot read the image"));
    size_t got = fread(magic, 1, sizeof magic, f);
    fclose(f);

    bool ok;
    if (got == 4 && memcmp(magic, "\x89PNG", 4) == 0)
        ok = load_png(path, img, err, err_size);
    else if (got == 4 && memcmp(magic, "GIF8", 4) == 0)
        ok = load_gif(path, img, err, err_size);
    else
        return fail(err, err_size, _("only PNG and GIF images are supported"));
    if (ok && !fit(img, side)) {
        kr_img_free(img);
        return fail(err, err_size, _("out of memory"));
    }
    return ok;
}

/* ---- the built-in shapes --------------------------------------------------- */

static bool inside(kr_shape shape, int side, int x, int y)
{
    if (x < 0 || y < 0 || x >= side || y >= side)
        return false;
    double r = side / 2.0, dx = x + 0.5 - r, dy = y + 0.5 - r;
    switch (shape) {
    case KR_SHAPE_CIRCLE:
        return dx * dx + dy * dy <= r * r;
    case KR_SHAPE_RING:
        return dx * dx + dy * dy <= r * r && dx * dx + dy * dy >= (0.55 * r) * (0.55 * r);
    case KR_SHAPE_SQUARE:
        return true;
    case KR_SHAPE_DIAMOND:
        return fabs(dx) + fabs(dy) <= r;
    case KR_SHAPE_CROSS: {
        double t = side / 10.0 < 1.0 ? 1.0 : side / 10.0;
        return fabs(dx) <= t || fabs(dy) <= t;
    }
    }
    return false;
}

bool kr_img_shape(kr_shape shape, int side, uint32_t color, bool has_rim, uint32_t rim, kr_img *img)
{
    memset(img, 0, sizeof *img);
    if (side < 1 || side > KR_IMG_MAX_SIDE)
        return false;
    img->px = calloc(1, sizeof *img->px);
    img->delay_ms = calloc(1, sizeof *img->delay_ms);
    uint32_t *px = calloc((size_t)side * (size_t)side, sizeof *px);
    if (!img->px || !img->delay_ms || !px) {
        free(px);
        kr_img_free(img);
        return false;
    }
    int rw = side >= 24 ? 2 : 1;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            if (!inside(shape, side, x, y))
                continue;
            bool edge = false;
            for (int oy = -rw; oy <= rw && has_rim && !edge; oy++)
                for (int ox = -rw; ox <= rw && !edge; ox++)
                    edge = !inside(shape, side, x + ox, y + oy);
            px[(size_t)y * (size_t)side + (size_t)x] = 0xff000000u | (edge ? rim : color);
        }
    img->px[0] = px;
    img->n = 1;
    img->w = img->h = side;
    return true;
}
