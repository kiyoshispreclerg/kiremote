/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"

struct kr_v4l2 {
    int fd;
    size_t size;
};

kr_v4l2 *kr_v4l2_open(const char *path, unsigned width, unsigned height, const char **why)
{
    static char reason[200];
    const char *file = getenv("KIREMOTE_V4L2_FILE");
    kr_v4l2 *v = calloc(1, sizeof *v);
    if (!v) {
        *why = _("Out of memory");
        return NULL;
    }
    v->size = (size_t)width * height * 3 / 2;
    if (file && *file) {
        v->fd = open(file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (v->fd < 0) {
            *why = strerror(errno);
            free(v);
            return NULL;
        }
        return v;
    }
    v->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (v->fd < 0) {
        snprintf(reason, sizeof reason, _("Could not open %s: %s"), path, strerror(errno));
        *why = reason;
        free(v);
        return NULL;
    }
    struct v4l2_capability cap;
    if (ioctl(v->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        snprintf(reason, sizeof reason, _("%s is not a video device"), path);
        *why = reason;
        goto fail;
    }
    /* DroidCam's copy of the module reports itself as "Droidcam", a real camera
     * as its own driver: writing to those is wrong at best. */
    if (strcmp((const char *)cap.driver, "v4l2 loopback") != 0) {
        snprintf(reason, sizeof reason, _("%s is not a v4l2loopback device (its driver is \"%s\")"), path,
                 (const char *)cap.driver);
        *why = reason;
        goto fail;
    }
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = width;
    fmt.fmt.pix.sizeimage = (uint32_t)v->size;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    if (ioctl(v->fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.width != width || fmt.fmt.pix.height != height ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
        snprintf(reason, sizeof reason, _("%s does not take %ux%u video: %s"), path, width, height,
                 errno ? strerror(errno) : "-");
        *why = reason;
        goto fail;
    }
    return v;
fail:
    close(v->fd);
    free(v);
    return NULL;
}

void kr_v4l2_close(kr_v4l2 *v)
{
    if (!v)
        return;
    close(v->fd);
    free(v);
}

size_t kr_v4l2_frame_size(const kr_v4l2 *v)
{
    return v->size;
}

bool kr_v4l2_write(kr_v4l2 *v, const uint8_t *frame)
{
    ssize_t n = write(v->fd, frame, v->size);
    if (n == (ssize_t)v->size)
        return true;
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return true; /* no room: this frame is dropped */
    if (n >= 0)
        return true; /* a short write of a loopback frame is the same */
    kr_log_debug("v4l2: %s", strerror(errno));
    return errno != ENODEV && errno != EIO && errno != EBADF && errno != EPIPE;
}
