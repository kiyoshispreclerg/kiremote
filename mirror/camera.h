/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CAMERA_H
#define KR_CAMERA_H

/* The phone's camera as a webcam: each decoded frame is scaled to the fixed
 * size the video device was opened with (libswscale; a change of the phone's
 * picture size, as when it is turned, is scaled to fit rather than reopening
 * the device, which programs already reading would not follow), optionally
 * flipped left to right like a mirror, and written out (v4l2.c). */

#include <stdbool.h>

#include "v4l2.h"

struct AVFrame;

typedef struct kr_camera kr_camera;

/* out is owned by the camera from here on. */
kr_camera *kr_camera_new(kr_v4l2 *out, unsigned width, unsigned height, bool flip);
void kr_camera_free(kr_camera *c);
/* false once the device is gone. */
bool kr_camera_frame(kr_camera *c, const struct AVFrame *frame);

#endif
