/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.share.request: files both ways (plugins/share/shareplugin.cpp
 * upstream; and/plugins/share for the Android side).
 *
 *   file: {"filename","lastModified"?,"creationTime"?,"open"?,
 *          "numberOfFiles","totalPayloadSize"} + a payload
 *   text: {"text"}   url: {"url"}
 *
 * Phase 1 receives files into the share folder and only logs text and
 * URLs; the clipboard and xdg-open come in phase 2. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "transfer.h"
#include "util.h"

#define SHARE "kdeconnect.share.request"
#define SHARE_UPDATE "kdeconnect.share.request.update"

static const char *const incoming[] = {SHARE, SHARE_UPDATE, NULL};
static const char *const outgoing[] = {SHARE, NULL};

/* The last path component; never empty, ".", ".." or hidden-only. */
static void clean_name(const char *in, char *out, size_t size)
{
    const char *slash = in ? strrchr(in, '/') : NULL;
    const char *base = slash ? slash + 1 : in ? in : "";
    if (!base[0] || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        snprintf(out, size, "%lld", (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        return;
    }
    snprintf(out, size, "%s", base);
}

static void receive_file(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    char dir[KR_PATH_MAX], name[256], path[KR_PATH_MAX];
    if (!kr_config_share_dir(kr_daemon_config(d), dir, sizeof dir)) {
        kr_log_warn(_("%s sent a file, but there is no folder to save it in"),
                    kr_device_name(dev));
        return;
    }
    clean_name(kr_packet_str(p, "filename"), name, sizeof name);
    if (!kr_pathf(path, sizeof path, "%s/%s", dir, name)) {
        kr_log_warn(_("file name too long: %s"), name);
        return;
    }
    int64_t mtime = 0;
    kr_packet_int(p, "lastModified", &mtime);

    if (p->payload_size != 0 && p->payload_port) {
        if (kr_daemon_download(d, dev, p, path, mtime))
            kr_log_info(_("receiving %s from %s"), name, kr_device_name(dev));
        return;
    }
    /* No payload: an empty file. */
    char final[KR_PATH_MAX];
    kr_make_parent_dirs(path);
    for (int n = 0; n < 1000; n++) {
        if (!kr_unique_path(path, n, final, sizeof final))
            break;
        int fd = open(final, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd >= 0) {
            close(fd);
            kr_log_info(_("received %s from %s"), final, kr_device_name(dev));
            return;
        }
        if (errno != EEXIST)
            break;
    }
    kr_log_warn(_("cannot save %s: %s"), path, strerror(errno));
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    if (strcmp(p->type, SHARE_UPDATE) == 0)
        return; /* totals for the progress, which we don't show yet */
    const char *text = kr_packet_str(p, "text");
    const char *url = kr_packet_str(p, "url");
    if (kr_packet_str(p, "filename") || p->payload_port)
        receive_file(d, dev, p);
    else if (text)
        kr_log_info(_("%s shared a text: %s"), kr_device_name(dev), text);
    else if (url)
        kr_log_info(_("%s shared a link: %s"), kr_device_name(dev), url);
}

int kr_share_files(kr_daemon *d, kr_device *dev, const cJSON *paths, cJSON *errors)
{
    int n_files = 0, sent = 0;
    int64_t total = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, paths)
    {
        struct stat st;
        if (cJSON_IsString(e) && stat(e->valuestring, &st) == 0 && S_ISREG(st.st_mode)) {
            n_files++;
            total += st.st_size;
        }
    }

    cJSON_ArrayForEach(e, paths)
    {
        char err[KR_PATH_MAX + 128];
        const char *path = cJSON_IsString(e) ? e->valuestring : "";
        struct stat st;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            snprintf(err, sizeof err, _("cannot send %s: %s"), path,
                     fd < 0 ? strerror(errno) : _("not a regular file"));
            cJSON_AddItemToArray(errors, cJSON_CreateString(err));
            if (fd >= 0)
                close(fd);
            continue;
        }
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;
        kr_packet *p = kr_packet_new(SHARE);
        if (!p) {
            close(fd);
            break;
        }
        cJSON_AddStringToObject(p->body, "filename", name);
        cJSON_AddNumberToObject(p->body, "lastModified",
                                (double)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000);
        cJSON_AddBoolToObject(p->body, "open", false);
        cJSON_AddNumberToObject(p->body, "numberOfFiles", n_files);
        cJSON_AddNumberToObject(p->body, "totalPayloadSize", (double)total);
        bool ok;
        if (st.st_size > 0) {
            ok = kr_daemon_upload(d, dev, p, fd, st.st_size, name);
        } else {
            close(fd);
            ok = kr_daemon_send(d, dev, p);
        }
        kr_packet_free(p);
        if (ok) {
            sent++;
        } else {
            snprintf(err, sizeof err, _("cannot send %s"), path);
            cJSON_AddItemToArray(errors, cJSON_CreateString(err));
        }
    }
    return sent;
}

const kr_plugin kr_plugin_share = {
    .name = "share",
    .incoming = incoming,
    .outgoing = outgoing,
    .on_packet = on_packet,
};
