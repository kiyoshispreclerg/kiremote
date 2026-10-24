/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_V4LINFO_H
#define KR_V4LINFO_H

/* Where the phone's camera can go: a v4l2loopback video device. What can be
 * known without root and without opening anything is read from /sys: whether
 * the v4l2loopback module is loaded (DroidCam's copy of it, v4l2loopback_dc,
 * is another module with its own devices, and not ours to write to), and
 * which virtual video nodes exist. The driver itself is checked for real
 * when the node is opened (v4l2.c). Used by the daemon, for the note on the
 * settings' Camera tab, and by kiremote-mirror, to choose the device. */

#include <stdbool.h>
#include <stddef.h>

#define KR_V4L_MAX_NODES 8

typedef struct {
    char path[64]; /* /dev/video10 */
    char name[64]; /* what /sys says it is called */
    bool writable; /* this user may open it for writing */
} kr_v4l_node;

typedef struct {
    bool module;    /* v4l2loopback is loaded */
    bool droidcam;  /* DroidCam's v4l2loopback_dc is loaded (its devices are not used) */
    kr_v4l_node nodes[KR_V4L_MAX_NODES]; /* virtual video nodes that may be ours */
    int n;
} kr_v4l;

/* Reads /sys and /dev. The roots are for the tests ("/sys", "/dev"). */
void kr_v4l_scan(kr_v4l *v);
void kr_v4l_scan_in(kr_v4l *v, const char *sys, const char *dev);

/* The device to write to: the configured one (any path), else the only
 * candidate. false with *why (translated) when there is no way to use it. */
bool kr_v4l_pick(const kr_v4l *v, const char *configured, char *out, size_t size, const char **why);

/* The paragraph for the settings: how things stand and, if the module is
 * missing, how to load it (as root). malloc'd, translated. */
char *kr_v4l_note(const kr_v4l *v, const char *configured);

#endif
