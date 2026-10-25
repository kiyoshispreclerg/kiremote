/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The presentation pointer's pixels: built-in shapes, PNG and GIF files
 * (with animation, transparency and scaling) and bad files. */
#define _DEFAULT_SOURCE /* mkdtemp */
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "pointer_img.h"

static char tmpdir[64];

static void make_tmpdir(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/kiremote-test-XXXXXX");
    if (!mkdtemp(tmpdir))
        abort();
}

static void remove_tmpdir(void)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", tmpdir);
}

static void put(const char *name, const void *data, size_t n, char *path, size_t size)
{
    snprintf(path, size, "%s/%s", tmpdir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n)
        abort();
    fclose(f);
}

TEST(color_parse)
{
    uint32_t c;
    CHECK(kr_color_parse("#ff8000", &c));
    CHECK_INT_EQ(c, 0xff8000);
    CHECK(kr_color_parse("#ABCDEF", &c));
    CHECK_INT_EQ(c, 0xabcdef);
    CHECK(!kr_color_parse("ff8000", &c));
    CHECK(!kr_color_parse("#ff800", &c));
    CHECK(!kr_color_parse("#ff80000", &c));
    CHECK(!kr_color_parse("#ff80zz", &c));
    CHECK(!kr_color_parse(NULL, &c));
}

TEST(circle_has_a_rim_and_clear_corners)
{
    kr_img img;
    CHECK(kr_img_shape(KR_SHAPE_CIRCLE, 20, 0xff0000, true, 0xffffff, &img));
    CHECK_INT_EQ(img.n, 1);
    CHECK_INT_EQ(img.w, 20);
    CHECK_INT_EQ(img.px[0][10 * 20 + 10], 0xffff0000u); /* middle */
    CHECK_INT_EQ(img.px[0][10 * 20 + 0] >> 24, 0xff);   /* left edge: rim */
    CHECK_INT_EQ(img.px[0][10 * 20 + 0] & 0xffffff, 0xffffff);
    CHECK_INT_EQ(img.px[0][0] >> 24, 0); /* corner: outside */
    kr_img_free(&img);

    CHECK(kr_img_shape(KR_SHAPE_CIRCLE, 20, 0xff0000, false, 0xffffff, &img));
    CHECK_INT_EQ(img.px[0][10 * 20 + 0] & 0xffffff, 0xff0000); /* no rim */
    kr_img_free(&img);
}

TEST(other_shapes)
{
    kr_img img;
    CHECK(kr_img_shape(KR_SHAPE_SQUARE, 16, 0x00ff00, false, 0, &img));
    CHECK_INT_EQ(img.px[0][0], 0xff00ff00u);
    CHECK_INT_EQ(img.px[0][16 * 16 - 1], 0xff00ff00u);
    kr_img_free(&img);

    CHECK(kr_img_shape(KR_SHAPE_RING, 40, 0x00ff00, false, 0, &img));
    CHECK_INT_EQ(img.px[0][20 * 40 + 20] >> 24, 0); /* hollow */
    CHECK_INT_EQ(img.px[0][20 * 40 + 2] >> 24, 0xff);
    kr_img_free(&img);

    CHECK(kr_img_shape(KR_SHAPE_DIAMOND, 20, 0x00ff00, false, 0, &img));
    CHECK_INT_EQ(img.px[0][0] >> 24, 0);
    CHECK_INT_EQ(img.px[0][10 * 20 + 10] >> 24, 0xff);
    kr_img_free(&img);

    CHECK(kr_img_shape(KR_SHAPE_CROSS, 40, 0x00ff00, false, 0, &img));
    CHECK_INT_EQ(img.px[0][0] >> 24, 0);
    CHECK_INT_EQ(img.px[0][20 * 40 + 0] >> 24, 0xff); /* the arm reaches the edge */
    CHECK_INT_EQ(img.px[0][0 * 40 + 20] >> 24, 0xff);
    kr_img_free(&img);

    CHECK(!kr_img_shape(KR_SHAPE_CIRCLE, 0, 0, false, 0, &img));
}

/* 2x2: red, transparent / half-transparent green, blue. */
static void write_png(const char *path)
{
    static const uint8_t px[16] = {255, 0, 0, 255, 0, 0, 0, 0, 0, 255, 0, 128, 0, 0, 255, 255};
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    im.width = im.height = 2;
    im.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&im, path, 0, px, 0, NULL))
        abort();
}

TEST(png_is_read_and_scaled)
{
    make_tmpdir();
    char path[128], err[160];
    snprintf(path, sizeof path, "%s/a.png", tmpdir);
    write_png(path);

    kr_img img;
    CHECK(kr_img_load(path, 2, &img, err, sizeof err));
    CHECK_INT_EQ(img.n, 1);
    CHECK_INT_EQ(img.w, 2);
    CHECK_INT_EQ(img.px[0][0], 0xffff0000u);
    CHECK_INT_EQ(img.px[0][1] >> 24, 0);
    CHECK_INT_EQ(img.px[0][2] >> 24, 128);
    CHECK_INT_EQ(img.px[0][3], 0xff0000ffu);
    kr_img_free(&img);

    CHECK(kr_img_load(path, 6, &img, err, sizeof err)); /* enlarged: nearest */
    CHECK_INT_EQ(img.w, 6);
    CHECK_INT_EQ(img.h, 6);
    CHECK_INT_EQ(img.px[0][0], 0xffff0000u);
    CHECK_INT_EQ(img.px[0][2 * 6 + 5] >> 24, 0);
    CHECK_INT_EQ(img.px[0][5 * 6 + 5], 0xff0000ffu);
    kr_img_free(&img);

    CHECK(kr_img_load(path, 1, &img, err, sizeof err)); /* shrunk: an average */
    CHECK_INT_EQ(img.w, 1);
    CHECK_INT_EQ(img.h, 1);
    CHECK(img.px[0][0] >> 24 > 100 && img.px[0][0] >> 24 < 200);
    kr_img_free(&img);
    remove_tmpdir();
}

TEST(gif_frames_delays_and_transparency)
{
    static const uint8_t gif[] = {
        'G', 'I', 'F', '8', '9', 'a', 2, 0, 1, 0, 0x80, 0, 0,
        0xff, 0, 0, 0, 0, 0xff, /* red, blue */
        0x21, 0xf9, 4, 0x05, 10, 0, 1, 0, /* 100 ms, index 1 is transparent */
        0x2c, 0, 0, 0, 0, 2, 0, 1, 0, 0, 2, 2, 0x44, 0x0a, 0, /* pixels 0 1 */
        0x21, 0xf9, 4, 0x04, 20, 0, 0, 0, /* 200 ms */
        0x2c, 0, 0, 0, 0, 2, 0, 1, 0, 0, 2, 2, 0x0c, 0x0a, 0, /* pixels 1 0 */
        0x3b};
    make_tmpdir();
    char path[128], err[160];
    put("a.gif", gif, sizeof gif, path, sizeof path);
    kr_img img;
    CHECK_MSG(kr_img_load(path, 2, &img, err, sizeof err), "%s", err);
    CHECK_INT_EQ(img.n, 2);
    CHECK_INT_EQ(img.w, 2);
    CHECK_INT_EQ(img.h, 1);
    CHECK_INT_EQ(img.delay_ms[0], 100);
    CHECK_INT_EQ(img.delay_ms[1], 200);
    CHECK_INT_EQ(img.px[0][0], 0xffff0000u);
    CHECK_INT_EQ(img.px[0][1] >> 24, 0);
    CHECK_INT_EQ(img.px[1][0], 0xff0000ffu);
    CHECK_INT_EQ(img.px[1][1], 0xffff0000u);
    kr_img_free(&img);
    remove_tmpdir();
}

TEST(bad_files_are_refused)
{
    make_tmpdir();
    char path[128], err[160];
    kr_img img;

    CHECK(!kr_img_load("/nonexistent/x.png", 10, &img, err, sizeof err));
    CHECK(err[0]);

    put("t.txt", "hello, not an image", 19, path, sizeof path);
    CHECK(!kr_img_load(path, 10, &img, err, sizeof err));
    CHECK(img.n == 0 && img.px == NULL);

    put("t.png", "\x89PNG\r\n\x1a\nnonsense", 16, path, sizeof path);
    CHECK(!kr_img_load(path, 10, &img, err, sizeof err));

    put("t.gif", "GIF89a\x02\x00\x01\x00\x80", 11, path, sizeof path); /* cut short */
    CHECK(!kr_img_load(path, 10, &img, err, sizeof err));

    CHECK(!kr_img_load(tmpdir, 10, &img, err, sizeof err)); /* a directory */
    remove_tmpdir();
}

int main(void)
{
    RUN(color_parse);
    RUN(circle_has_a_rim_and_clear_corners);
    RUN(other_shapes);
    RUN(png_is_read_and_scaled);
    RUN(gif_frames_delays_and_transparency);
    RUN(bad_files_are_refused);
    return harness_report();
}
