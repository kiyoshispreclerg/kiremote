/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.share.request: files, text and links, both ways
 * (plugins/share/shareplugin.cpp upstream; and/plugins/share for Android).
 *
 *   file: {"filename","lastModified"?,"creationTime"?,"open"?,
 *          "numberOfFiles","totalPayloadSize"} + a payload
 *   text: {"text"}   url: {"url"}
 *
 * Files go to the share folder. Text goes to the clipboard, with a
 * notification that offers "Open Link" when its last line is a web link.
 * A link is opened with xdg-open; unlike the upstream (which opens
 * whatever it is given), only http, https, ftp, mailto, tel, sms and geo
 * links are, and the notification says it happened. */
#include <errno.h>
#include <strings.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "proc.h"
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

/* A link we may hand to xdg-open: a known scheme, one line, no spaces. */
bool kr_url_openable(const char *url)
{
    static const char *const schemes[] = {"http", "https", "ftp", "mailto", "tel", "sms", "geo"};
    size_t n = strcspn(url, ":");
    if (url[n] != ':' || n == 0 || strlen(url) > 4000)
        return false;
    bool known = false;
    for (size_t i = 0; i < sizeof schemes / sizeof *schemes; i++)
        known |= strlen(schemes[i]) == n && strncasecmp(url, schemes[i], n) == 0;
    for (const char *c = url; known && *c; c++)
        if ((unsigned char)*c <= ' ' || *c == 0x7f)
            known = false;
    return known;
}

static void open_url(kr_daemon *d, const char *url)
{
    if (!kr_url_openable(url)) {
        kr_log_warn(_("not opening a link with an unknown kind of address"));
        return;
    }
    const char *argv[] = {"xdg-open", url, NULL};
    if (!kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL))
        kr_log_warn(_("cannot run xdg-open: %s"), strerror(errno));
}

typedef struct {
    kr_daemon *d;
    char url[1024];
} link_ctx;

static void link_action(void *ud, const char *key)
{
    link_ctx *c = ud;
    if (strcmp(key, "open") == 0)
        open_url(c->d, c->url);
}

static void link_closed(void *ud, int reason)
{
    (void)reason;
    free(ud);
}

/* The last non-empty line of text, if it is a link we would open. */
static bool last_line_link(const char *text, char *out, size_t size)
{
    const char *end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
        end--;
    const char *start = end;
    while (start > text && start[-1] != '\n')
        start--;
    if ((size_t)(end - start) >= size || end == start)
        return false;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return kr_url_openable(out);
}

static void receive_text(kr_daemon *d, kr_device *dev, const char *text)
{
    bool copied = kr_clipboard_set_text(d, text);
    char title[256];
    snprintf(title, sizeof title, copied ? _("Shared text from %s copied to clipboard") : _("Shared text from %s"),
             kr_device_name(dev));
    link_ctx *c = calloc(1, sizeof *c);
    const char *actions[3] = {"open", _("Open Link"), NULL};
    bool is_link = c && last_line_link(text, c->url, sizeof c->url);
    if (c)
        c->d = d;
    kr_note *n = kr_notify_show(kr_daemon_notify(d),
                                &(kr_note_spec){.summary = title, .body = text, .icon = "edit-paste",
                                                .actions = is_link ? actions : NULL, .timeout_ms = -1},
                                c ? &(kr_note_cb){.on_action = link_action, .on_closed = link_closed} : NULL, c);
    if (!n)
        free(c);
}

static void receive_url(kr_daemon *d, kr_device *dev, const char *url)
{
    char title[256];
    snprintf(title, sizeof title, _("Link from %s"), kr_device_name(dev));
    if (kr_url_openable(url)) {
        kr_note *n = kr_notify_show(kr_daemon_notify(d),
                                    &(kr_note_spec){.summary = title, .body = url, .icon = "internet-web-browser",
                                                    .timeout_ms = -1},
                                    NULL, NULL);
        if (n)
            kr_note_detach(n);
    }
    open_url(d, url);
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
        receive_text(d, dev, text);
    else if (url)
        receive_url(d, dev, url);
}

/* "share-text" and "share-url" on the control socket. */
static bool request(kr_daemon *d, const char *type, const cJSON *req, cJSON *reply)
{
    bool is_text = strcmp(type, "share-text") == 0;
    if (!is_text && strcmp(type, "share-url") != 0)
        return false;
    kr_device *dev = kr_daemon_request_device(d, req, reply);
    if (!dev)
        return true;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, is_text ? "text" : "url");
    if (!cJSON_IsString(v) || !v->valuestring[0]) {
        kr_control_fail(reply, is_text ? _("missing \"text\"") : _("missing \"url\""));
        return true;
    }
    kr_packet *p = kr_packet_new(SHARE);
    if (p)
        cJSON_AddStringToObject(p->body, is_text ? "text" : "url", v->valuestring);
    if (!p || !kr_daemon_send(d, dev, p))
        kr_control_fail(reply, kr_device_paired(dev) ? _("the device is not reachable or does not take this")
                                                     : _("the device is not paired"));
    kr_packet_free(p);
    return true;
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
    .request = request,
};
