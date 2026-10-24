/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_V4L2_H
#define KR_V4L2_H

/* Frames into a v4l2loopback device: the other programs read them as a
 * webcam. Planar YUV 4:2:0 (V4L2's YU12), one frame per write(), the size
 * fixed when the device is opened. Never blocks: a frame the device has no
 * room for is dropped, as a live camera would. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kr_v4l2 kr_v4l2;

/* The size must be even. NULL with *why (translated) if the node cannot be
 * opened, is not a v4l2loopback device (DroidCam's, a real camera...) or
 * takes no such format. For the tests, $KIREMOTE_V4L2_FILE makes it a plain
 * file the frames are appended to, and nothing is asked of the device. */
kr_v4l2 *kr_v4l2_open(const char *path, unsigned width, unsigned height, const char **why);
void kr_v4l2_close(kr_v4l2 *v);

/* Bytes of one frame: width * height * 3 / 2. */
size_t kr_v4l2_frame_size(const kr_v4l2 *v);
/* false only if the device failed for good (it went away). */
bool kr_v4l2_write(kr_v4l2 *v, const uint8_t *frame);

#endif
