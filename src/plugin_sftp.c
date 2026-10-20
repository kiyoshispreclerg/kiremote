/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Browse the phone's files (plugins/sftp upstream; and/plugins/sftp for the
 * server, an SSH server inside the Android app).
 *
 *   out kdeconnect.sftp.request   {"startBrowsing": true}
 *   in  kdeconnect.sftp           {"user", "port", "password", "path",
 *                                  "multiPaths"?: [...], "pathNames"?: [...]}
 *                                 the server is up (the phone also sends "ip",
 *                                 which we ignore: the address of the link is
 *                                 the one to trust)
 *                                 {"serverRunning": false}  it was restarted
 *                                 {"errorMessage": "..."}   it could not start
 *
 * Control socket: "browse-files" asks the phone to start its server, mounts
 * it with sshfs under $XDG_RUNTIME_DIR/kiremote-sftp-<id> and opens that
 * folder with xdg-open; "unmount-files" undoes it. Like the upstream, sshfs
 * logs in with our private key (the phone only takes the key of the device
 * it is paired with); unlike it, the password is not passed on and ssh never
 * asks for one (BatchMode). Without sshfs the phone's sftp:// address, with
 * the password, goes to xdg-open and the file manager does the rest.
 *
 * sshfs runs in the foreground as a child of the loop; the mount is ready
 * when its folder is on another device than the parent's, which is checked
 * every 0.2 s. [sftp] sshfs = PROGRAM in the .ini picks another program. With multiPaths the whole phone is mounted from "/" and the
 * first storage (the internal one) is what gets opened. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "util.h"

#define SFTP "kdeconnect.sftp"
#define SFTP_REQUEST "kdeconnect.sftp.request"
#define ASK_TIMEOUT_S 10.0
#define MOUNT_TIMEOUT_S 15.0
#define POLL_S 0.2

static const char *const incoming[] = {SFTP, NULL};
static const char *const outgoing[] = {SFTP_REQUEST, NULL};

typedef struct {
    char id[64];
    char name[96];
    bool asked;      /* startBrowsing sent, the answer is awaited */
    bool mounting;   /* sshfs started, the mount is not there yet */
    bool mounted;
    bool restart;    /* sshfs is being stopped to start again with job */
    kr_proc *proc;
    double deadline; /* give up on asked/mounting at this time */
    double next;     /* look at the mount again at this time */
    double gone_at;  /* the device went away: unmount if still away then */
    bool used;
    char mp[KR_PATH_MAX];
    char open_path[KR_PATH_MAX];
    char last[200];  /* last line sshfs wrote */
    struct {
        bool set;
        char user[64], ip[INET_ADDRSTRLEN], remote[KR_PATH_MAX], open_path[KR_PATH_MAX];
        int port;
    } job;
} sftp_state;

#define MAX_MOUNTS 8
#define AWAY_GRACE_S 60.0

/* Not in the device's own state: that is dropped whenever the link is
 * replaced (the phone reconnecting), and sshfs talks to the phone's SSH
 * server directly, not over the link, so a link that flaps must not take the
 * mount down. A device that stays away for AWAY_GRACE_S loses it. */
static sftp_state mounts[MAX_MOUNTS];
static kr_daemon *the_daemon;
static bool source_on;

/* ---- what the phone may hand us ---- */

bool kr_sftp_user_ok(const char *user)
{
    size_t n = user ? strlen(user) : 0;
    if (n == 0 || n > 32 || user[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)user[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              c == '.' || c == '-'))
            return false;
    }
    return true;
}

bool kr_sftp_path_ok(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) > 1024 || !kr_utf8_valid(path, strlen(path)))
        return false;
    for (const char *c = path; *c; c++)
        if ((unsigned char)*c < ' ' || *c == 0x7f)
            return false;
    for (const char *s = path; *s;) {
        size_t n = strcspn(s + 1, "/") + 1; /* "/part" */
        if ((n == 3 && s[1] == '.' && s[2] == '.'))
            return false;
        s += n;
    }
    return true;
}

/* Appends s percent-encoded (RFC 3986 unreserved characters stay; keep_slash
 * also leaves "/"); false if it does not fit. */
static bool encode(char *out, size_t size, size_t *at, const char *s, bool keep_slash)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
                     c == '.' || c == '_' || c == '~' || (keep_slash && c == '/');
        int w = plain ? snprintf(out + *at, size - *at, "%c", c) : snprintf(out + *at, size - *at, "%%%02X", c);
        if (w < 0 || (size_t)w >= size - *at)
            return false;
        *at += (size_t)w;
    }
    return true;
}

bool kr_sftp_url(char *out, size_t size, const char *user, const char *password, const char *ip, int port,
                 const char *path)
{
    size_t at = 0;
    int w = snprintf(out, size, "sftp://");
    if (w < 0 || (size_t)w >= size)
        goto fail;
    at = (size_t)w;
    if (!encode(out, size, &at, user, false))
        goto fail;
    if (password && *password) {
        if (at + 1 >= size)
            goto fail;
        out[at++] = ':';
        if (!encode(out, size, &at, password, false))
            goto fail;
    }
    w = snprintf(out + at, size - at, "@%s:%d", ip, port);
    if (w < 0 || (size_t)w >= size - at)
        goto fail;
    at += (size_t)w;
    if (!encode(out, size, &at, path, true))
        goto fail;
    return true;
fail:
    if (size)
        out[0] = '\0';
    return false;
}

/* ---- helpers ---- */

static sftp_state *state_of(kr_device *dev)
{
    const char *id = kr_device_id(dev);
    sftp_state *free_slot = NULL;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && strcmp(mounts[i].id, id) == 0)
            return &mounts[i];
        if (!mounts[i].used && !free_slot)
            free_slot = &mounts[i];
    }
    if (!free_slot)
        return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    snprintf(free_slot->id, sizeof free_slot->id, "%s", id);
    snprintf(free_slot->name, sizeof free_slot->name, "%s", kr_device_name(dev));
    char name[96];
    snprintf(name, sizeof name, "kiremote-sftp-%s", free_slot->id);
    if (!kr_runtime_path(free_slot->mp, sizeof free_slot->mp, name))
        free_slot->mp[0] = '\0';
    return free_slot;
}

/* Nothing running or awaited: the slot can go to another device. */
static void release_if_idle(sftp_state *st)
{
    if (!st->proc && !st->asked && !st->mounting && !st->mounted && !st->job.set && !st->restart)
        st->used = false;
}

static void tell_failure(kr_daemon *d, const sftp_state *st, const char *why)
{
    char body[512];
    kr_notify *n = kr_daemon_notify(d);
    kr_notify_escape(n, why, body, sizeof body);
    kr_log_warn(_("%s: cannot browse the files: %s"), st->name, why);
    kr_note *note = kr_notify_show(n,
                                   &(kr_note_spec){.summary = st->name,
                                                   .body = body,
                                                   .icon = "dialog-error",
                                                   .timeout_ms = -1},
                                   NULL, NULL);
    if (note)
        kr_note_detach(note);
}

static void open_folder(kr_daemon *d, const char *target)
{
    const char *argv[] = {"xdg-open", target, NULL};
    if (!kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL))
        kr_log_warn(_("cannot run xdg-open: %s"), strerror(errno));
}

/* fusermount -u on a mount point; lazy, since sshfs may have died. */
static void free_mountpoint(kr_daemon *d, const char *mp)
{
    const char *tool = kr_which("fusermount3") ? "fusermount3" : "fusermount";
    const char *argv[] = {tool, "-u", "-z", "-q", mp, NULL};
    kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL);
}

static bool is_mounted(const char *mp)
{
    char parent[KR_PATH_MAX + 4];
    struct stat a, b;
    return kr_pathf(parent, sizeof parent, "%s/..", mp) && stat(mp, &a) == 0 && stat(parent, &b) == 0 &&
           a.st_dev != b.st_dev;
}

static void note_line(void *ud, const char *line)
{
    sftp_state *st = ud;
    if (line[0])
        snprintf(st->last, sizeof st->last, "%s", line);
    kr_log_debug("sshfs (%s): %s", st->name, line);
}

static void start_sshfs(kr_daemon *d, sftp_state *st);

/* The program that mounts: [sftp] sshfs = ... (a name in PATH, or a path). */
static const char *sshfs_program(kr_daemon *d)
{
    const char *p = kr_config_get(kr_daemon_config(d), "sftp", "sshfs");
    return p && *p ? p : "sshfs";
}

static void sshfs_exited(void *ud, int status, const char *out, size_t len)
{
    (void)out;
    (void)len;
    sftp_state *st = ud;
    kr_daemon *d = the_daemon;
    bool wanted_up = st->mounting || st->mounted;
    st->proc = NULL;
    bool was_mounting = st->mounting;
    st->mounting = st->mounted = false;
    st->next = 0;
    if (st->restart) {
        st->restart = false;
        start_sshfs(d, st);
        return;
    }
    free_mountpoint(d, st->mp);
    release_if_idle(st);
    if (was_mounting) {
        char why[300];
        snprintf(why, sizeof why, _("sshfs failed: %s"), st->last[0] ? st->last : _("no message"));
        tell_failure(d, st, why);
    } else if (wanted_up) {
        kr_log_info(_("%s: the phone's files were unmounted (status %d)"), st->name, status);
    }
}

static bool prepare_mountpoint(sftp_state *st)
{
    if (mkdir(st->mp, 0700) < 0 && errno != EEXIST)
        return false;
    /* Not somebody else's folder (the runtime dir may be /tmp). A mount
     * point whose sshfs died shows up as ENOTCONN; free it first. */
    struct stat sb;
    if (stat(st->mp, &sb) < 0 && errno == ENOTCONN) {
        free_mountpoint(the_daemon, st->mp);
        return true; /* the mount below fails with a message if it is still there */
    }
    return lstat(st->mp, &sb) == 0 && S_ISDIR(sb.st_mode) && sb.st_uid == getuid();
}

static void start_sshfs(kr_daemon *d, sftp_state *st)
{
    if (!st->job.set)
        return;
    char key[KR_PATH_MAX], spec[256 + KR_PATH_MAX], port[16], idopt[KR_PATH_MAX + 16], uid[40], gid[40];
    if (!kr_config_path(key, sizeof key, "privateKey.pem") || strchr(key, ',') ||
        !kr_pathf(idopt, sizeof idopt, "IdentityFile=%s", key)) {
        tell_failure(d, st, _("the path of the private key cannot be given to sshfs"));
        st->job.set = false;
        return;
    }
    if (!prepare_mountpoint(st)) {
        tell_failure(d, st, _("cannot prepare the folder to mount the phone's files"));
        st->job.set = false;
        return;
    }
    snprintf(spec, sizeof spec, "%s@%s:%s", st->job.user, st->job.ip, st->job.remote);
    snprintf(port, sizeof port, "%d", st->job.port);
    snprintf(uid, sizeof uid, "uid=%lu", (unsigned long)getuid());
    snprintf(gid, sizeof gid, "gid=%lu", (unsigned long)getgid());
    snprintf(st->open_path, sizeof st->open_path, "%s", st->job.open_path);
    /* -s: one thread, or file chunks arrive out of order and get corrupted
     * (the upstream's comment). -F /dev/null: not the user's ~/.ssh/config. */
    const char *argv[] = {sshfs_program(d), spec, st->mp, "-p", port, "-s", "-f", "-F", "/dev/null",
                          "-o", idopt, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                          "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                          "-o", uid, "-o", gid, "-o", "reconnect", "-o", "ServerAliveInterval=30", NULL};
    st->last[0] = '\0';
    st->job.set = false;
    kr_log_info(_("mounting the files of %s at %s"), st->name, st->mp);
    st->proc = kr_proc_spawn_ex(kr_daemon_procs(d), argv, KR_PROC_USER | KR_PROC_STDERR,
                                &(kr_proc_cb){.on_line = note_line, .on_exit = sshfs_exited}, st);
    if (!st->proc) {
        tell_failure(d, st, strerror(errno));
        return;
    }
    st->mounting = true;
    st->deadline = kr_now() + MOUNT_TIMEOUT_S;
    st->next = kr_now() + POLL_S;
}

/* Stops sshfs (it unmounts itself on SIGTERM) and forgets it. */
static void stop(kr_daemon *d, sftp_state *st)
{
    st->asked = st->job.set = st->restart = false;
    st->gone_at = 0;
    if (st->proc) {
        kr_log_info(_("unmounting the files of %s"), st->name);
        kr_proc_kill(st->proc);
        kr_proc_detach(st->proc);
        st->proc = NULL;
        free_mountpoint(d, st->mp);
    }
    st->mounting = st->mounted = false;
    st->next = st->deadline = 0;
    release_if_idle(st);
}

/* ---- the loop ---- */

static void each_state(void (*fn)(kr_daemon *, kr_device *, sftp_state *, void *), void *arg)
{
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used)
            fn(the_daemon, NULL, &mounts[i], arg);
}

static void tick(kr_daemon *d, kr_device *dev, sftp_state *st, void *arg)
{
    (void)dev;
    (void)arg;
    double now = kr_now();
    if (st->gone_at && now >= st->gone_at) {
        kr_log_info(_("%s has been away for a while"), st->name);
        stop(d, st);
        return;
    }
    if (st->asked && now >= st->deadline) {
        st->asked = false;
        st->deadline = 0;
        tell_failure(d, st, _("the device is not responding (is the permission to see files granted?)"));
    }
    if (st->mounting && st->proc && now >= st->next) {
        if (is_mounted(st->mp)) {
            st->mounting = false;
            st->mounted = true;
            st->next = st->deadline = 0;
            kr_log_info(_("%s: files mounted at %s"), st->name, st->mp);
            open_folder(d, st->open_path);
        } else if (now >= st->deadline) {
            kr_proc_kill(st->proc); /* sshfs_exited tells why */
            if (!st->last[0])
                snprintf(st->last, sizeof st->last, "%s", _("timed out"));
            st->next = 0;
        } else {
            st->next = now + POLL_S;
        }
    }
}

static void earliest(kr_daemon *d, kr_device *dev, sftp_state *st, void *arg)
{
    (void)d;
    (void)dev;
    double *t = arg;
    double w = st->mounting ? st->next : st->asked ? st->deadline : 0;
    if (st->gone_at && (!w || st->gone_at < w))
        w = st->gone_at;
    if (w && (!*t || w < *t))
        *t = w;
}

static double src_deadline(void *ud)
{
    (void)ud;
    double t = 0;
    each_state(earliest, &t);
    return t;
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    (void)ud;
    (void)fds;
    (void)n;
    each_state(tick, NULL);
}

static void init(kr_daemon *d)
{
    the_daemon = d;
    source_on = kr_daemon_add_source(d, &(kr_source){NULL, src_dispatch, src_deadline}, &source_on);
}

static void shutdown_plugin(kr_daemon *d)
{
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used)
            stop(d, &mounts[i]);
    if (source_on)
        kr_daemon_remove_source(d, &source_on);
    source_on = false;
    the_daemon = NULL;
}

/* ---- packets ---- */

static bool read_port(const kr_packet *p, int *port)
{
    int64_t n;
    if (!kr_packet_int(p, "port", &n) || n < 1 || n > 65535)
        return false;
    *port = (int)n;
    return true;
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    sftp_state *st = state_of(dev);
    if (!st)
        return;
    const cJSON *running = cJSON_GetObjectItemCaseSensitive(p->body, "serverRunning");
    if (running) {
        /* The server went down: if we have it mounted, ask for a new one. */
        if (cJSON_IsFalse(running) && (st->mounted || st->mounting)) {
            kr_log_info(_("%s: the file server restarted"), st->name);
            kr_packet *r = kr_packet_new(SFTP_REQUEST);
            if (r) {
                cJSON_AddBoolToObject(r->body, "startBrowsing", true);
                st->asked = kr_daemon_send(d, dev, r);
                st->deadline = kr_now() + ASK_TIMEOUT_S;
            }
            kr_packet_free(r);
        }
        return;
    }
    const char *err = kr_packet_str(p, "errorMessage");
    if (err) {
        if (st->asked || st->mounting) {
            st->asked = false;
            tell_failure(d, st, kr_utf8_valid(err, strlen(err)) ? err : "?");
        }
        return;
    }
    if (!st->asked) {
        kr_log_debug("%s: unexpected sftp packet, ignoring", st->name);
        return;
    }
    const char *user = kr_packet_str(p, "user"), *pass = kr_packet_str(p, "password");
    const char *path = kr_packet_str(p, "path");
    int port;
    if (!user || !pass || !path || !read_port(p, &port)) {
        kr_log_warn(_("%s sent an incomplete sftp packet"), st->name);
        return;
    }
    char ip[INET_ADDRSTRLEN];
    if (!kr_device_address(dev, ip, sizeof ip)) {
        tell_failure(d, st, _("the device has no network address"));
        st->asked = false;
        return;
    }
    if (!kr_sftp_user_ok(user)) {
        kr_log_warn(_("%s sent a user name we will not use"), st->name);
        st->asked = false;
        return;
    }

    /* Several storages: mount "/" and open the first one. */
    const char *open_rel = "";
    const cJSON *multi = cJSON_GetObjectItemCaseSensitive(p->body, "multiPaths");
    if (cJSON_IsArray(multi)) {
        const cJSON *first = cJSON_GetArrayItem(multi, 0);
        if (cJSON_IsString(first) && kr_sftp_path_ok(first->valuestring))
            open_rel = first->valuestring;
        path = "/";
    } else if (!kr_sftp_path_ok(path)) {
        kr_log_warn(_("%s sent a path we will not use"), st->name);
        st->asked = false;
        return;
    }
    st->asked = false;
    st->deadline = 0;

    if (!kr_which(sshfs_program(d))) {
        char url[2048];
        if (!kr_sftp_url(url, sizeof url, user, pass, ip, port, open_rel[0] ? open_rel : path)) {
            tell_failure(d, st, _("the address is too long"));
            return;
        }
        kr_log_info(_("sshfs is not installed: opening the phone's address instead"));
        open_folder(d, url);
        return;
    }

    snprintf(st->job.user, sizeof st->job.user, "%s", user);
    snprintf(st->job.ip, sizeof st->job.ip, "%s", ip);
    snprintf(st->job.remote, sizeof st->job.remote, "%s", path);
    st->job.port = port;
    st->job.set = true;
    if (!st->mp[0] || !kr_pathf(st->job.open_path, sizeof st->job.open_path, "%s%s", st->mp, open_rel)) {
        tell_failure(d, st, _("the path is too long"));
        st->job.set = false;
        return;
    }
    if (st->proc) {
        st->restart = true; /* start again once it has exited */
        kr_proc_kill(st->proc);
    } else {
        start_sshfs(d, st);
    }
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    (void)d;
    sftp_state *st = state_of(dev);
    if (st) {
        snprintf(st->name, sizeof st->name, "%s", kr_device_name(dev));
        st->gone_at = 0; /* it is back */
    }
}

static void on_disconnect(kr_daemon *d, kr_device *dev)
{
    sftp_state *st = state_of(dev);
    if (!st)
        return;
    st->asked = false; /* the answer would not come over this link */
    if (st->proc) {
        kr_log_info(_("%s went away; its files stay mounted for %d s"), st->name, (int)AWAY_GRACE_S);
        st->gone_at = kr_now() + AWAY_GRACE_S;
    } else {
        stop(d, st);
    }
}

/* ---- control socket ---- */

static bool request(kr_daemon *d, const char *type, const cJSON *req, cJSON *reply)
{
    bool browse = strcmp(type, "browse-files") == 0;
    if (!browse && strcmp(type, "unmount-files") != 0)
        return false;
    kr_device *dev = kr_daemon_request_device(d, req, reply);
    if (!dev)
        return true;
    sftp_state *st = state_of(dev);
    if (!st) {
        kr_control_fail(reply, _("the device is not reachable or does not take this"));
        return true;
    }
    if (!browse) {
        stop(d, st);
        return true;
    }
    if (st->mounted) {
        open_folder(d, st->open_path);
        return true;
    }
    if (st->mounting || st->asked)
        return true; /* already on its way */
    kr_packet *p = kr_packet_new(SFTP_REQUEST);
    if (p)
        cJSON_AddBoolToObject(p->body, "startBrowsing", true);
    bool ok = p && kr_daemon_send(d, dev, p);
    kr_packet_free(p);
    if (!ok) {
        kr_control_fail(reply, kr_device_paired(dev) ? _("the device is not reachable or does not take this")
                                                     : _("the device is not paired"));
        return true;
    }
    st->asked = true;
    st->deadline = kr_now() + ASK_TIMEOUT_S;
    return true;
}

const kr_plugin kr_plugin_sftp = {
    .name = "sftp",
    .incoming = incoming,
    .outgoing = outgoing,
    .init = init,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_disconnect = on_disconnect,
    .on_packet = on_packet,
    .request = request,
};
