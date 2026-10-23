/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The scrcpy-server on the phone, started step by step (server.h). The
 * steps follow upstream/scrcpy/app/src/server.c (execute_server,
 * sc_server_connect_to) and adb/adb_tunnel.c (the reverse tunnel: we
 * listen first, so there is no retrying to connect). */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include "adb.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

/* Where the jar goes on the phone: not scrcpy's own path, so a scrcpy of
 * another version on the same phone does not swap ours (docs/UPSTREAM.md). */
#define DEVICE_JAR "/data/local/tmp/kiremote-scrcpy-server.jar"

typedef enum { ST_DEVICES, ST_PUSH, ST_REVERSE, ST_WAIT, ST_RUNNING, ST_DONE } state;

struct kr_server {
    kr_procs *ps;
    kr_server_cb cb;
    void *ud;
    kr_server_opts o;
    char jar[KR_PATH_MAX];
    char codec[8];
    char want[128];   /* the serial asked for, or "" */
    char serial[128], model[64];
    char socket_name[32]; /* scrcpy_<scid> */
    state st;
    kr_proc *step;  /* the adb command running now */
    kr_proc *shell; /* the server */
    bool reverse_on;
    int listen_fd;
    double deadline;
    char last_line[256]; /* the server's, to say why it ended */
};

static void fail(kr_server *s, const char *msg)
{
    if (s->st == ST_DONE)
        return;
    s->st = ST_DONE;
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    s->deadline = 0;
    s->cb.on_error(s->ud, msg);
}

/* adb's own words (its last line), or ours. */
static void fail_with_output(kr_server *s, const char *ours, const char *out, size_t len)
{
    char buf[512];
    const char *line = NULL;
    size_t llen = 0;
    /* The last non-empty line. */
    for (size_t i = 0; i < len;) {
        size_t n = strcspn(out + i, "\n");
        if (n > 0 && !(n == 1 && out[i] == '\r')) {
            line = out + i;
            llen = n;
        }
        i += n + 1;
    }
    if (line) {
        while (llen && line[llen - 1] == '\r')
            llen--;
        snprintf(buf, sizeof buf, "%s (%.*s)", ours, (int)llen, line);
        fail(s, buf);
    } else {
        fail(s, ours);
    }
}

static kr_proc *adb(kr_server *s, const char *const *args, const kr_proc_cb *cb, unsigned flags)
{
    const char *argv[32];
    int n = 0;
    argv[n++] = kr_adb_program();
    if (s->serial[0]) {
        argv[n++] = "-s";
        argv[n++] = s->serial;
    }
    for (; *args && n < 31; args++)
        argv[n++] = *args;
    argv[n] = NULL;
    return kr_proc_spawn_ex(s->ps, argv, flags, cb, s);
}

static void step_failed_to_start(kr_server *s)
{
    char buf[256];
    snprintf(buf, sizeof buf, _("Could not run %s: %s"), kr_adb_program(), strerror(errno));
    fail(s, buf);
}

/* ---- 4. the server --------------------------------------------------------- */

static void shell_line(void *ud, const char *line)
{
    kr_server *s = ud;
    if (!*line)
        return;
    snprintf(s->last_line, sizeof s->last_line, "%s", line);
    if (s->cb.on_log)
        s->cb.on_log(s->ud, line);
}

static void shell_exit(void *ud, int status, const char *out, size_t len)
{
    kr_server *s = ud;
    (void)status;
    (void)out;
    (void)len;
    s->shell = NULL;
    if (s->last_line[0]) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s (%s)", _("The server on the phone stopped"), s->last_line);
        fail(s, buf);
    } else {
        fail(s, _("The server on the phone stopped"));
    }
}

static void start_shell(kr_server *s)
{
    char scid[32], max_size[32], bit_rate[32], fps[32], codec[32], cp[KR_PATH_MAX];
    snprintf(scid, sizeof scid, "scid=%s", s->socket_name + strlen("scrcpy_"));
    snprintf(cp, sizeof cp, "CLASSPATH=%s", DEVICE_JAR);
    snprintf(codec, sizeof codec, "video_codec=%s", s->codec);
    const char *args[24] = {"shell", cp, "app_process", "/", "com.genymobile.scrcpy.Server", KR_SCRCPY_VERSION,
                            scid,    kr_log_verbose() ? "log_level=debug" : "log_level=info",
                            "audio=false", "control=false", codec};
    int n = 11;
    if (s->o.max_size) {
        snprintf(max_size, sizeof max_size, "max_size=%u", s->o.max_size);
        args[n++] = max_size;
    }
    if (s->o.bit_rate) {
        snprintf(bit_rate, sizeof bit_rate, "video_bit_rate=%u", s->o.bit_rate);
        args[n++] = bit_rate;
    }
    if (s->o.max_fps) {
        snprintf(fps, sizeof fps, "max_fps=%u", s->o.max_fps);
        args[n++] = fps;
    }
    args[n] = NULL;
    static const kr_proc_cb cb = {shell_line, shell_exit};
    s->shell = adb(s, args, &cb, KR_PROC_STDERR);
    if (!s->shell) {
        step_failed_to_start(s);
        return;
    }
    s->st = ST_WAIT;
    s->deadline = kr_now() + (s->o.timeout > 0 ? s->o.timeout : 10);
}

/* ---- 3. the tunnel --------------------------------------------------------- */

static void reverse_exit(void *ud, int status, const char *out, size_t len)
{
    kr_server *s = ud;
    s->step = NULL;
    if (status != 0) {
        fail_with_output(s, _("Could not open the adb tunnel to the phone"), out, len);
        return;
    }
    s->reverse_on = true;
    start_shell(s);
}

static void open_tunnel(kr_server *s)
{
    uint32_t scid = 0;
    if (getrandom(&scid, sizeof scid, 0) != sizeof scid)
        scid = (uint32_t)getpid() ^ (uint32_t)(kr_now() * 1e6);
    scid &= 0x7FFFFFFF; /* 31 bits, as scrcpy_generate_scid */
    snprintf(s->socket_name, sizeof s->socket_name, "scrcpy_%08x", scid);

    s->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t alen = sizeof a;
    if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(s->listen_fd, 1) < 0 ||
        getsockname(s->listen_fd, (struct sockaddr *)&a, &alen) < 0) {
        char buf[256];
        snprintf(buf, sizeof buf, _("Could not open a local port: %s"), strerror(errno));
        fail(s, buf);
        return;
    }
    char remote[64], local[32];
    snprintf(remote, sizeof remote, "localabstract:%s", s->socket_name);
    snprintf(local, sizeof local, "tcp:%u", (unsigned)ntohs(a.sin_port));
    const char *args[] = {"reverse", remote, local, NULL};
    static const kr_proc_cb cb = {NULL, reverse_exit};
    s->st = ST_REVERSE;
    s->step = adb(s, args, &cb, KR_PROC_CAPTURE | KR_PROC_STDERR);
    if (!s->step)
        step_failed_to_start(s);
}

static void remove_tunnel(kr_server *s)
{
    if (!s->reverse_on)
        return;
    s->reverse_on = false;
    char remote[64];
    snprintf(remote, sizeof remote, "localabstract:%s", s->socket_name);
    const char *args[] = {"reverse", "--remove", remote, NULL};
    kr_proc *p = adb(s, args, NULL, 0); /* fire and forget */
    (void)p;
}

/* ---- 2. the jar ------------------------------------------------------------ */

static void push_exit(void *ud, int status, const char *out, size_t len)
{
    kr_server *s = ud;
    s->step = NULL;
    if (status != 0) {
        fail_with_output(s, _("Could not copy the server to the phone"), out, len);
        return;
    }
    open_tunnel(s);
}

/* ---- 1. the phone ---------------------------------------------------------- */

static void devices_exit(void *ud, int status, const char *out, size_t len)
{
    kr_server *s = ud;
    s->step = NULL;
    if (status != 0 || !out) {
        fail_with_output(s, _("adb could not list the devices"), out ? out : "", out ? len : 0);
        return;
    }
    kr_adb_dev devs[16];
    int n = kr_adb_parse_devices(out, devs, 16);
    const char *why = NULL;
    int i = kr_adb_pick(devs, n, s->o.serial, &why);
    if (i < 0) {
        fail(s, why);
        return;
    }
    snprintf(s->serial, sizeof s->serial, "%s", devs[i].serial);
    snprintf(s->model, sizeof s->model, "%s", devs[i].model);
    kr_log_debug("adb: %s (%s)", s->serial, s->model);

    const char *args[] = {"push", s->jar, DEVICE_JAR, NULL};
    static const kr_proc_cb cb = {NULL, push_exit};
    s->st = ST_PUSH;
    s->step = adb(s, args, &cb, KR_PROC_CAPTURE | KR_PROC_STDERR);
    if (!s->step)
        step_failed_to_start(s);
}

kr_server *kr_server_start(kr_procs *ps, const kr_server_opts *opts, const kr_server_cb *cb, void *ud)
{
    kr_server *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->ps = ps;
    s->cb = *cb;
    s->ud = ud;
    s->o = *opts;
    s->listen_fd = -1;
    snprintf(s->jar, sizeof s->jar, "%s", opts->jar);
    snprintf(s->codec, sizeof s->codec, "%s", opts->codec && *opts->codec ? opts->codec : "h264");
    s->o.jar = s->jar;
    s->o.codec = s->codec;
    snprintf(s->want, sizeof s->want, "%s", opts->serial ? opts->serial : "");
    s->o.serial = s->want;

    const char *args[] = {"devices", "-l", NULL};
    static const kr_proc_cb pcb = {NULL, devices_exit};
    s->st = ST_DEVICES;
    s->step = adb(s, args, &pcb, KR_PROC_CAPTURE | KR_PROC_STDERR);
    if (!s->step) {
        step_failed_to_start(s); /* on_error before this returns */
    }
    return s;
}

void kr_server_free(kr_server *s)
{
    if (!s)
        return;
    if (s->step) {
        kr_proc_kill(s->step);
        kr_proc_detach(s->step);
    }
    if (s->shell) {
        kr_proc_kill(s->shell);
        kr_proc_detach(s->shell);
    }
    remove_tunnel(s);
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    free(s);
}

int kr_server_pollfds(kr_server *s, struct pollfd *fds, int max)
{
    if (s->st != ST_WAIT || s->listen_fd < 0 || max < 1)
        return 0;
    fds[0] = (struct pollfd){.fd = s->listen_fd, .events = POLLIN};
    return 1;
}

void kr_server_dispatch(kr_server *s, const struct pollfd *fds, int n)
{
    if (s->st != ST_WAIT || s->listen_fd < 0)
        return;
    for (int i = 0; i < n; i++) {
        if (fds[i].fd != s->listen_fd || !fds[i].revents)
            continue;
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            char buf[256];
            snprintf(buf, sizeof buf, _("Could not accept the server's connection: %s"), strerror(errno));
            fail(s, buf);
            return;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK); /* as promised in server.h */
        /* Only one socket (video): the tunnel is no longer needed. */
        close(s->listen_fd);
        s->listen_fd = -1;
        s->deadline = 0;
        remove_tunnel(s);
        s->st = ST_RUNNING;
        s->cb.on_ready(s->ud, fd, s->serial, s->model);
        return;
    }
}

double kr_server_deadline(const kr_server *s)
{
    return s->st == ST_WAIT ? s->deadline : 0;
}

void kr_server_tick(kr_server *s)
{
    if (s->st == ST_WAIT && s->deadline && kr_now() >= s->deadline) {
        if (s->last_line[0]) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s (%s)", _("The server on the phone did not connect"), s->last_line);
            fail(s, buf);
        } else {
            fail(s, _("The server on the phone did not connect"));
        }
    }
}

bool kr_server_find_jar(char *out, size_t size)
{
    const char *env = getenv("KIREMOTE_SCRCPY_SERVER");
    if (env && *env)
        return kr_pathf(out, size, "%s", env) && kr_file_exists(out);
    char exe[KR_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            if (kr_pathf(out, size, "%s/scrcpy-server", exe) && kr_file_exists(out))
                return true;
        }
    }
    return kr_pathf(out, size, "%s/kiremote/scrcpy-server", KR_DATADIR) && kr_file_exists(out);
}
