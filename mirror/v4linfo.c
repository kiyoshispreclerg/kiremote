/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "v4linfo.h"

#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"
#include "util.h"

/* A module parameter listing device numbers ("10,11", "-1"): is n one? */
static bool param_has(const char *sys, const char *module, int n)
{
    char path[KR_PATH_MAX], text[128];
    if (!kr_pathf(path, sizeof path, "%s/module/%s/parameters/video_nr", sys, module))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = 0;
    for (char *p = text; *p;) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p)
            break;
        if (v == n)
            return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void kr_v4l_scan_in(kr_v4l *v, const char *sys, const char *dev)
{
    memset(v, 0, sizeof *v);
    char path[KR_PATH_MAX];
    if (kr_pathf(path, sizeof path, "%s/module/v4l2loopback", sys))
        v->module = is_dir(path);
    if (kr_pathf(path, sizeof path, "%s/module/v4l2loopback_dc", sys))
        v->droidcam = is_dir(path);
    if (!kr_pathf(path, sizeof path, "%s/class/video4linux", sys))
        return;
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && v->n < KR_V4L_MAX_NODES) {
        int num;
        char tail;
        if (sscanf(e->d_name, "video%d%c", &num, &tail) != 1 || num < 0)
            continue;
        /* Virtual: a camera on USB or PCI lives under its bus. */
        char entry[KR_PATH_MAX], real[PATH_MAX];
        if (!kr_pathf(entry, sizeof entry, "%s/class/video4linux/%s", sys, e->d_name) || !realpath(entry, real))
            continue;
        if (!strstr(real, "/devices/virtual/"))
            continue;
        /* DroidCam's devices are its own. */
        if (v->droidcam && param_has(sys, "v4l2loopback_dc", num))
            continue;
        kr_v4l_node *nd = &v->nodes[v->n];
        if (!kr_pathf(nd->path, sizeof nd->path, "%s/video%d", dev, num) || access(nd->path, F_OK) != 0)
            continue;
        nd->name[0] = 0;
        char name_path[KR_PATH_MAX];
        if (kr_pathf(name_path, sizeof name_path, "%s/name", entry)) {
            FILE *f = fopen(name_path, "r");
            if (f) {
                if (fgets(nd->name, sizeof nd->name, f))
                    nd->name[strcspn(nd->name, "\n")] = 0;
                fclose(f);
            }
        }
        nd->writable = access(nd->path, W_OK) == 0;
        v->n++;
    }
    closedir(d);
    /* readdir's order is nobody's: the lowest number first. */
    for (int i = 1; i < v->n; i++)
        for (int j = i; j > 0 && strcmp(v->nodes[j - 1].path, v->nodes[j].path) > 0; j--) {
            kr_v4l_node t = v->nodes[j];
            v->nodes[j] = v->nodes[j - 1];
            v->nodes[j - 1] = t;
        }
}

void kr_v4l_scan(kr_v4l *v)
{
    kr_v4l_scan_in(v, "/sys", "/dev");
}

bool kr_v4l_pick(const kr_v4l *v, const char *configured, char *out, size_t size, const char **why)
{
    if (configured && *configured) {
        if (access(configured, W_OK) != 0) {
            *why = access(configured, F_OK) == 0
                       ? _("This user cannot write to the video device (join the group \"video\")")
                       : _("The video device does not exist (is the v4l2loopback module loaded?)");
            return false;
        }
        return kr_pathf(out, size, "%s", configured);
    }
    if (!v->module) {
        *why = _("The v4l2loopback module is not loaded (see Camera in the settings)");
        return false;
    }
    if (v->n == 0) {
        *why = _("The v4l2loopback module has no device (load it with video_nr=10, see Camera in the settings)");
        return false;
    }
    if (v->n > 1) {
        *why = _("There is more than one video device: choose one in the Camera settings");
        return false;
    }
    if (!v->nodes[0].writable) {
        *why = _("This user cannot write to the video device (join the group \"video\")");
        return false;
    }
    return kr_pathf(out, size, "%s", v->nodes[0].path);
}

static char *dup_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static char *dup_printf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s)
        vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

char *kr_v4l_note(const kr_v4l *v, const char *configured)
{
    char path[KR_PATH_MAX];
    const char *why = NULL;
    if (configured && *configured) { /* the user's word goes whatever the module is */
        if (kr_v4l_pick(v, configured, path, sizeof path, &why))
            return dup_printf(_("Ready: the camera will appear as %s."), path);
        return dup_printf(_("Cannot use %s: %s."), configured, why);
    }
    if (!v->module) {
        return strdup(v->droidcam
            ? _("The v4l2loopback module is not loaded. The one that is, v4l2loopback_dc, is DroidCam's and "
                "cannot be used here.\n"
                "Load it once per boot, as root (it asks for your password):\n"
                "  sudo modprobe v4l2loopback video_nr=10 card_label=Kiremote exclusive_caps=1\n"
                "To have it at every boot, put \"v4l2loopback\" in /etc/modules-load.d/v4l2loopback.conf and "
                "\"options v4l2loopback video_nr=10 card_label=Kiremote exclusive_caps=1\" in "
                "/etc/modprobe.d/v4l2loopback.conf. The package is v4l2loopback-dkms.")
            : _("The v4l2loopback module is not loaded.\n"
                "Load it once per boot, as root (it asks for your password):\n"
                "  sudo modprobe v4l2loopback video_nr=10 card_label=Kiremote exclusive_caps=1\n"
                "To have it at every boot, put \"v4l2loopback\" in /etc/modules-load.d/v4l2loopback.conf and "
                "\"options v4l2loopback video_nr=10 card_label=Kiremote exclusive_caps=1\" in "
                "/etc/modprobe.d/v4l2loopback.conf. The package is v4l2loopback-dkms."));
    }
    if (kr_v4l_pick(v, NULL, path, sizeof path, &why))
        return dup_printf(_("Ready: the camera will appear as %s."), path);
    return dup_printf(_("The v4l2loopback module is loaded, but: %s."), why);
}
