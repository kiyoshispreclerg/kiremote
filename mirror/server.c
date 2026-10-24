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

typedef enum { ST_DEVICES, ST_ROUTE, ST_TCPIP, ST_CONNECT, ST_RETRY, ST_PUSH, ST_REVERSE, ST_WAIT, ST_RUNNING, ST_DONE } state;

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
    int accepted[3]; /* video, audio, control: as many as are expected, in this order */
    int n_accepted;
    double deadline;
    char target[128];    /* HOST:PORT being connected to */
    int tries;           /* `adb connect` attempts left */
    bool connect_tried;  /* the network connection was made (or given up) */
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

static void begin_connect(kr_server *s, int tries);
static void route_exit(void *ud, int status, const char *out, size_t len);
static void push_exit(void *ud, int status, const char *out, size_t len);
static void start_push(kr_server *s);

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
    const char *args[28] = {"shell", cp, "app_process", "/", "com.genymobile.scrcpy.Server", KR_SCRCPY_VERSION,
                            scid,    kr_log_verbose() ? "log_level=debug" : "log_level=info",
                            s->o.audio ? "audio=true" : "audio=false", codec};
    int n = 10;
    if (s->o.audio)
        args[n++] = "audio_codec=raw"; /* PCM: nothing to decode, and USB or Wi-Fi carry 1.5 Mbit/s */
    if (s->o.stay_awake)
        args[n++] = "stay_awake=true";
    if (s->o.view_only)
        args[n++] = "control=false";
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
    if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(s->listen_fd, 2) < 0 ||
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

/* ---- 0. the network ---------------------------------------------------------- */

static void list_devices(kr_server *s);

static void start_push(kr_server *s)
{
    const char *args[] = {"push", s->jar, DEVICE_JAR, NULL};
    static const kr_proc_cb cb = {NULL, push_exit};
    s->st = ST_PUSH;
    s->step = adb(s, args, &cb, KR_PROC_CAPTURE | KR_PROC_STDERR);
    if (!s->step)
        step_failed_to_start(s);
}

static void connect_exit(void *ud, int status, const char *out, size_t len)
{
    kr_server *s = ud;
    s->step = NULL;
    if (status == 0 && out && kr_adb_connect_ok(out)) {
        kr_log_debug("adb: connected to %s", s->target);
        s->connect_tried = true;
        snprintf(s->want, sizeof s->want, "%s", s->target); /* the phone to pick now */
        s->o.serial = s->want;
        list_devices(s);
        return;
    }
    if (--s->tries > 0) {
        s->st = ST_RETRY;
        s->deadline = kr_now() + 0.25;
        return;
    }
    char ours[256];
    snprintf(ours, sizeof ours, _("Could not connect to %s"), s->target);
    fail_with_output(s, ours, out ? out : "", out ? len : 0);
}

static void do_connect(kr_server *s)
{
    const char *args[] = {"connect", s->target, NULL};
    static const kr_proc_cb cb = {NULL, connect_exit};
    s->serial[0] = 0; /* adb connect is not about one device */
    s->st = ST_CONNECT;
    s->deadline = 0;
    s->step = adb(s, args, &cb, KR_PROC_CAPTURE | KR_PROC_STDERR);
    if (!s->step)
        step_failed_to_start(s);
}

static void begin_connect(kr_server *s, int tries)
{
    s->connect_tried = true; /* once, whatever comes of it */
    if (!kr_adb_host_port(s->o.connect, s->target, sizeof s->target)) {
        fail(s, _("That is not an address adb can connect to"));
        return;
    }
    s->tries = tries;
    do_connect(s);
}

/* The phone is on USB: its address on Wi-Fi, then adbd listens on the network. */
static void tcpip_exit(void *ud, int status, const char *out, size_t len)
{
    kr_server *s = ud;
    s->step = NULL;
    if (status != 0) {
        fail_with_output(s, _("Could not switch the phone to Wi-Fi debugging"), out ? out : "", out ? len : 0);
        return;
    }
    /* adbd restarts: it takes a moment to listen (scrcpy waits up to 10 s). */
    s->tries = 40;
    s->st = ST_RETRY;
    s->deadline = kr_now() + 0.5;
}

static void route_exit(void *ud, int status, const char *out, size_t len)
{
    kr_server *s = ud;
    s->step = NULL;
    char ip[32];
    if (status != 0 || !out || !kr_adb_parse_device_ip(out, ip, sizeof ip)) {
        (void)len;
        fail(s, _("The phone is not on Wi-Fi (no address found)"));
        return;
    }
    snprintf(s->target, sizeof s->target, "%s:%d", ip, KR_ADB_TCPIP_PORT);
    s->connect_tried = true;
    kr_log_debug("adb: the phone is at %s", s->target);
    char port[16];
    snprintf(port, sizeof port, "%d", KR_ADB_TCPIP_PORT);
    const char *args[] = {"tcpip", port, NULL};
    static const kr_proc_cb cb = {NULL, tcpip_exit};
    s->st = ST_TCPIP;
    s->step = adb(s, args, &cb, KR_PROC_CAPTURE | KR_PROC_STDERR);
    if (!s->step)
        step_failed_to_start(s);
}

/* ---- 1. the phone ---------------------------------------------------------- */

static void devices_exit(void *ud, int status, const char *out, size_t len);

static void list_devices(kr_server *s)
{
    const char *args[] = {"devices", "-l", NULL};
    static const kr_proc_cb pcb = {NULL, devices_exit};
    s->serial[0] = 0;
    s->st = ST_DEVICES;
    s->deadline = 0;
    s->step = adb(s, args, &pcb, KR_PROC_CAPTURE | KR_PROC_STDERR);
    if (!s->step)
        step_failed_to_start(s); /* on_error before this returns */
}

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
    if (n == 0 && !s->connect_tried && s->o.connect && *s->o.connect) {
        begin_connect(s, 1); /* no cable: the network, if we were told where */
        return;
    }
    const char *why = NULL;
    int i = kr_adb_pick(devs, n, s->o.serial, &why);
    if (i < 0) {
        fail(s, why);
        return;
    }
    snprintf(s->serial, sizeof s->serial, "%s", devs[i].serial);
    snprintf(s->model, sizeof s->model, "%s", devs[i].model);
    kr_log_debug("adb: %s (%s)", s->serial, s->model);

    if (s->o.tcpip && !s->connect_tried && !kr_adb_is_tcpip(s->serial)) {
        const char *args[] = {"shell", "ip", "route", NULL};
        static const kr_proc_cb rcb = {NULL, route_exit};
        s->st = ST_ROUTE;
        s->step = adb(s, args, &rcb, KR_PROC_CAPTURE | KR_PROC_STDERR);
        if (!s->step)
            step_failed_to_start(s);
        return;
    }
    start_push(s);
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

    if (s->o.connect && *s->o.connect && !s->o.connect_fallback)
        begin_connect(s, 1);
    else
        list_devices(s);
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
    for (int i = 0; i < s->n_accepted; i++)
        close(s->accepted[i]);
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
        /* Video first, then audio, then control (the server's order). */
        int expected = 1 + (s->o.audio ? 1 : 0) + (s->o.view_only ? 0 : 1);
        s->accepted[s->n_accepted++] = fd;
        if (s->n_accepted < expected)
            return;
        int video = s->accepted[0], audio = s->o.audio ? s->accepted[1] : -1;
        int control = s->o.view_only ? -1 : s->accepted[expected - 1];
        s->n_accepted = 0; /* the caller owns them now */
        if (control >= 0) {
            /* Small and urgent: no Nagle (sc_server_connect_to). */
            int one = 1;
            setsockopt(control, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        /* All connected: the tunnel is no longer needed. */
        close(s->listen_fd);
        s->listen_fd = -1;
        s->deadline = 0;
        remove_tunnel(s);
        s->st = ST_RUNNING;
        s->cb.on_ready(s->ud, video, audio, control, s->serial, s->model);
        return;
    }
}

double kr_server_deadline(const kr_server *s)
{
    return s->st == ST_WAIT || s->st == ST_RETRY ? s->deadline : 0;
}

void kr_server_tick(kr_server *s)
{
    if (s->st == ST_RETRY && s->deadline && kr_now() >= s->deadline) {
        do_connect(s);
        return;
    }
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
