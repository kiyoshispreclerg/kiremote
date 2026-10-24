/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Which video devices the camera can go to, from a made-up /sys and /dev. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../harness.h"
#include "util.h"
#include "v4linfo.h"

static char root[64];

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void sh(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd) != 0)
        abort();
}

/* /sys/devices/virtual/video4linux/videoN (a loopback) or
 * /sys/devices/pci0000:00/usb1/1-1/video4linux/videoN (a real camera). */
static void node(int n, const char *name, bool virt)
{
    sh("mkdir -p %s/sys/devices/%s/video%d && echo '%s' > %s/sys/devices/%s/video%d/name && "
       "mkdir -p %s/sys/class/video4linux && ln -sfn ../../devices/%s/video%d %s/sys/class/video4linux/video%d && "
       "mkdir -p %s/dev && touch %s/dev/video%d",
       root, virt ? "virtual/video4linux" : "pci0000:00/usb1/video4linux", n, name, root,
       virt ? "virtual/video4linux" : "pci0000:00/usb1/video4linux", n, root,
       virt ? "virtual/video4linux" : "pci0000:00/usb1/video4linux", n, root, n, root, root, n);
}

static void fresh(void)
{
    snprintf(root, sizeof root, "/tmp/kiremote-v4l-XXXXXX");
    if (!mkdtemp(root))
        abort();
}

static void scan(kr_v4l *v)
{
    char s[96], d[96];
    snprintf(s, sizeof s, "%s/sys", root);
    snprintf(d, sizeof d, "%s/dev", root);
    kr_v4l_scan_in(v, s, d);
}

static void cleanup(void)
{
    sh("rm -rf %s", root);
}

TEST(no_module_says_how_to_load_it)
{
    fresh();
    node(0, "Integrated Camera", false); /* a real one: never ours */
    kr_v4l v;
    scan(&v);
    CHECK(!v.module && !v.droidcam);
    CHECK_INT_EQ(v.n, 0);
    char out[64];
    const char *why = NULL;
    CHECK(!kr_v4l_pick(&v, NULL, out, sizeof out, &why));
    CHECK_STR_HAS(why, "not loaded");
    char *note = kr_v4l_note(&v, NULL);
    CHECK_STR_HAS(note, "sudo modprobe v4l2loopback");
    CHECK_STR_HAS(note, "password");
    CHECK(!strstr(note, "DroidCam"));
    free(note);
    cleanup();
}

TEST(droidcams_module_is_not_ours)
{
    fresh();
    sh("mkdir -p %s/sys/module/v4l2loopback_dc/parameters && echo 0 > %s/sys/module/v4l2loopback_dc/parameters/video_nr",
       root, root);
    node(0, "Loopback video device", true);
    kr_v4l v;
    scan(&v);
    CHECK(!v.module && v.droidcam);
    CHECK_INT_EQ(v.n, 0); /* video0 is DroidCam's */
    char *note = kr_v4l_note(&v, NULL);
    CHECK_STR_HAS(note, "DroidCam");
    CHECK_STR_HAS(note, "sudo modprobe");
    free(note);
    cleanup();
}

TEST(a_loaded_module_with_one_device_is_picked)
{
    fresh();
    sh("mkdir -p %s/sys/module/v4l2loopback_dc/parameters && echo 0 > %s/sys/module/v4l2loopback_dc/parameters/video_nr",
       root, root);
    sh("mkdir -p %s/sys/module/v4l2loopback", root);
    node(0, "Droidcam", true);
    node(10, "Kiremote", true);
    node(1, "Webcam", false);
    kr_v4l v;
    scan(&v);
    CHECK(v.module && v.droidcam);
    CHECK_INT_EQ(v.n, 1);
    CHECK_STR_HAS(v.nodes[0].path, "/dev/video10");
    CHECK_STR_EQ(v.nodes[0].name, "Kiremote");
    char out[64];
    const char *why = NULL;
    CHECK(kr_v4l_pick(&v, NULL, out, sizeof out, &why));
    CHECK_STR_EQ(out, v.nodes[0].path);
    char *note = kr_v4l_note(&v, NULL);
    CHECK_STR_HAS(note, "Ready");
    CHECK_STR_HAS(note, "/dev/video10");
    free(note);
    cleanup();
}

TEST(several_or_none_need_the_user)
{
    fresh();
    sh("mkdir -p %s/sys/module/v4l2loopback", root);
    kr_v4l v;
    scan(&v);
    char out[64];
    const char *why = NULL;
    CHECK(!kr_v4l_pick(&v, NULL, out, sizeof out, &why));
    CHECK_STR_HAS(why, "no device");
    node(10, "A", true);
    node(11, "B", true);
    scan(&v);
    CHECK_INT_EQ(v.n, 2);
    CHECK_STR_HAS(v.nodes[0].path, "video10"); /* in order */
    CHECK(!kr_v4l_pick(&v, NULL, out, sizeof out, &why));
    CHECK_STR_HAS(why, "more than one");
    /* The user's choice settles it, and is checked. */
    char mine[96];
    snprintf(mine, sizeof mine, "%s/dev/video11", root);
    CHECK(kr_v4l_pick(&v, mine, out, sizeof out, &why));
    CHECK_STR_EQ(out, mine);
    CHECK(!kr_v4l_pick(&v, "/nonexistent/video9", out, sizeof out, &why));
    CHECK_STR_HAS(why, "does not exist");
    char *note = kr_v4l_note(&v, "/nonexistent/video9");
    CHECK_STR_HAS(note, "Cannot use /nonexistent/video9");
    free(note);
    cleanup();
}

int main(void)
{
    RUN(no_module_says_how_to_load_it);
    RUN(droidcams_module_is_not_ours);
    RUN(a_loaded_module_with_one_device_is_picked);
    RUN(several_or_none_need_the_user);
    return harness_report();
}
