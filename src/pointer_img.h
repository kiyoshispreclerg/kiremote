/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_POINTER_IMG_H
#define KR_POINTER_IMG_H

/* What the presentation pointer looks like, as pixels: either an image the
 * user chose (PNG, or a GIF with or without animation) or one of the built-in
 * shapes drawn here. Both end as the same thing, a list of frames of
 * 0xAARRGGBB pixels of one size, so the plugin only has one way to put them
 * on the screen. No X in here: it can be tested without a display.
 *
 * Files come from the user's own configuration, but they are still parsed by
 * libpng and giflib with limits (file size, dimensions, frame count). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KR_IMG_MAX_FILE (8u << 20)
#define KR_IMG_MAX_SIDE 4096
#define KR_IMG_MAX_FRAMES 200
#define KR_IMG_MAX_PIXELS (64u << 20) /* all frames together, before scaling */

typedef struct {
    int w, h;       /* every frame has this size */
    int n;          /* frames, at least 1 */
    uint32_t **px;  /* n arrays of w*h pixels, 0xAARRGGBB, not premultiplied */
    int *delay_ms;  /* n entries; meaningless when n == 1 */
} kr_img;

typedef enum { KR_SHAPE_CIRCLE, KR_SHAPE_RING, KR_SHAPE_SQUARE, KR_SHAPE_DIAMOND, KR_SHAPE_CROSS } kr_shape;

/* Reads path and scales it to fit in a box of side x side pixels, keeping
 * the proportions. On failure returns false with a (translated) reason in
 * err and img is empty. */
bool kr_img_load(const char *path, int side, kr_img *img, char *err, size_t err_size);

/* A shape of side x side pixels filled with color (0xRRGGBB), with a 1 or 2
 * pixel rim of rim (0xRRGGBB) if has_rim. One frame. */
bool kr_img_shape(kr_shape shape, int side, uint32_t color, bool has_rim, uint32_t rim, kr_img *img);

void kr_img_free(kr_img *img);

/* "#rrggbb" (any case) → 0xRRGGBB. */
bool kr_color_parse(const char *text, uint32_t *rgb);

#endif
