/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiremote-mirror: the phone's screen in a window, through the
 * scrcpy-server and adb (docs/PROTOCOL.md, "Espelhamento"). One process per
 * mirrored phone, alive only while the window is open; one thread, one
 * poll() loop: the adb commands, the video socket and X. */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libavutil/frame.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "decode.h"
#include "i18n.h"
#include "log.h"
#include "proc.h"
#include "screen.h"
#include "server.h"
#include "stream.h"
#include "util.h"

static void usage(FILE *out)
{
    fprintf(out, _("Usage: %s [OPTION]...\n"
                   "Shows the phone's screen in a window (USB debugging must be on).\n"
                   "\n"
                   "  -s, --serial SERIAL   the phone, as adb names it (default: the only one)\n"
                   "  -m, --max-size N      longer side of the video, in pixels (default: the phone's)\n"
                   "  -b, --bit-rate N      video bit rate, in bits per second\n"
                   "      --max-fps N       frames per second at most\n"
                   "      --codec CODEC     h264 (default), h265 or av1\n"
                   "  -v, --verbose         debug messages (and the server's)\n"
                   "  -V, --version         show the version and exit\n"
                   "  -h, --help            show this help and exit\n"),
            "kiremote-mirror");
}

typedef struct {
    kr_procs *procs;
    kr_server *server;
    int video_fd;
    kr_stream *stream;
    kr_decoder *dec;
    kr_screen *screen;
    char name[KR_STREAM_NAME_SIZE];
    char model[64];
    unsigned vid_w, vid_h;
    bool done;
    int status; /* exit status */
} mirror;

static void stop(mirror *m, int status, const char *msg)
{
    if (msg)
        kr_log_error("%s", msg);
    if (!m->done)
        m->status = status;
    m->done = true;
}

static void on_name(void *ud, const char *name)
{
    mirror *m = ud;
    snprintf(m->name, sizeof m->name, "%s", kr_utf8_valid(name, strlen(name)) ? name : "");
}

static void on_codec(void *ud, uint32_t codec)
{
    mirror *m = ud;
    kr_log_debug("video: %s", kr_codec_name(codec));
    m->dec = kr_decoder_new(codec);
    if (!m->dec)
        stop(m, 1, _("This computer cannot decode the phone's video (no decoder in libavcodec)"));
}

static void on_session(void *ud, uint32_t w, uint32_t h, bool resized)
{
    mirror *m = ud;
    (void)resized;
    kr_log_debug("video: %ux%u", w, h);
    m->vid_w = w;
    m->vid_h = h;
    if (m->screen) {
        kr_screen_video_size(m->screen, w, h);
        return;
    }
    const char *title = m->name[0] ? m->name : m->model[0] ? m->model : "Android";
    const char *why = NULL;
    m->screen = kr_screen_new(title, w, h, &why);
    if (!m->screen)
        stop(m, 1, why);
}

static void on_frame(void *ud, const AVFrame *f)
{
    mirror *m = ud;
    if (m->screen)
        kr_screen_frame(m->screen, f);
}

static void on_packet(void *ud, const uint8_t *data, size_t len, int64_t pts, bool key)
{
    mirror *m = ud;
    (void)key;
    if (m->done || !m->dec || pts == KR_STREAM_NO_PTS)
        return;
    if (!kr_decoder_push(m->dec, data, len, pts, on_frame, m))
        stop(m, 1, _("The video could not be decoded"));
}

static void server_ready(void *ud, int fd, const char *serial, const char *model)
{
    mirror *m = ud;
    kr_log_info(_("Mirroring %s"), model && *model ? model : serial);
    snprintf(m->model, sizeof m->model, "%s", model ? model : "");
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    m->video_fd = fd;
    static const kr_stream_cbs cbs = {on_name, on_codec, on_session, on_packet};
    m->stream = kr_stream_new(true, true, &cbs, m);
    if (!m->stream)
        stop(m, 1, _("Out of memory"));
}

static void server_error(void *ud, const char *message)
{
    stop(ud, 1, message);
}

static void server_log(void *ud, const char *line)
{
    (void)ud;
    /* The server's own log ("[server] INFO: ..."): only with -v. */
    kr_log_debug("%s", line);
}

static void read_video(mirror *m)
{
    static uint8_t buf[256 * 1024];
    for (int round = 0; round < 8 && !m->done; round++) {
        ssize_t n = read(m->video_fd, buf, sizeof buf);
        if (n > 0) {
            if (!kr_stream_feed(m->stream, buf, (size_t)n))
                stop(m, 1, kr_stream_error(m->stream));
            continue;
        }
        if (n == 0)
            stop(m, 0, _("The phone closed the connection"));
        else if (errno != EAGAIN && errno != EINTR)
            stop(m, 1, strerror(errno));
        return;
    }
}

static int open_signal_fd(void)
{
    signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        return -1;
    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

static bool parse_uint(const char *s, unsigned *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end || v > 0xFFFFFFFFul)
        return false;
    *out = (unsigned)v;
    return true;
}

int main(int argc, char **argv)
{
    kr_i18n_init();
    kr_log_init("kiremote-mirror");

    enum { OPT_MAX_FPS = 256, OPT_CODEC };
    static const struct option longopts[] = {
        {"serial", required_argument, NULL, 's'},
        {"max-size", required_argument, NULL, 'm'},
        {"bit-rate", required_argument, NULL, 'b'},
        {"max-fps", required_argument, NULL, OPT_MAX_FPS},
        {"codec", required_argument, NULL, OPT_CODEC},
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    kr_server_opts opts = {0};
    int c;
    while ((c = getopt_long(argc, argv, "s:m:b:vVh", longopts, NULL)) != -1) {
        bool ok = true;
        switch (c) {
        case 's':
            opts.serial = optarg;
            break;
        case 'm':
            ok = parse_uint(optarg, &opts.max_size) && opts.max_size <= KR_STREAM_MAX_SIDE;
            break;
        case 'b':
            ok = parse_uint(optarg, &opts.bit_rate);
            break;
        case OPT_MAX_FPS:
            ok = parse_uint(optarg, &opts.max_fps);
            break;
        case OPT_CODEC:
            opts.codec = optarg;
            ok = !strcmp(optarg, "h264") || !strcmp(optarg, "h265") || !strcmp(optarg, "av1");
            break;
        case 'v':
            kr_log_set_verbose(true);
            break;
        case 'V':
            printf("kiremote-mirror %s (scrcpy-server %s)\n", KR_VERSION, KR_SCRCPY_VERSION);
            return 0;
        case 'h':
            usage(stdout);
            return 0;
        default:
            ok = false;
            break;
        }
        if (!ok) {
            usage(stderr);
            return 2;
        }
    }
    if (optind < argc) {
        usage(stderr);
        return 2;
    }

    char jar[KR_PATH_MAX];
    if (!kr_server_find_jar(jar, sizeof jar)) {
        kr_log_error(_("scrcpy-server %s not found (scripts/fetch-scrcpy-server.sh downloads it)"),
                     KR_SCRCPY_VERSION);
        return 1;
    }
    opts.jar = jar;
    int sfd = open_signal_fd();
    if (sfd < 0) {
        kr_log_error(_("cannot watch signals: %s"), strerror(errno));
        return 1;
    }

    mirror m = {.video_fd = -1};
    m.procs = kr_procs_new();
    static const kr_server_cb scb = {server_ready, server_error, server_log};
    if (m.procs)
        m.server = kr_server_start(m.procs, &opts, &scb, &m);
    if (!m.server)
        stop(&m, 1, _("Out of memory"));

    enum { MAX_FDS = 80 };
    struct pollfd fds[MAX_FDS];
    while (!m.done) {
        int n = 0;
        fds[n++] = (struct pollfd){.fd = sfd, .events = POLLIN};
        int x_at = -1, v_at = -1;
        if (m.screen) {
            x_at = n;
            fds[n++] = (struct pollfd){.fd = kr_screen_fd(m.screen), .events = POLLIN};
        }
        if (m.video_fd >= 0) {
            v_at = n;
            fds[n++] = (struct pollfd){.fd = m.video_fd, .events = POLLIN};
        }
        int s_at = n;
        n += kr_server_pollfds(m.server, fds + n, MAX_FDS - n);
        int p_at = n;
        n += kr_procs_pollfds(m.procs, fds + n, MAX_FDS - n);

        double next = kr_server_deadline(m.server), pd = kr_procs_deadline(m.procs);
        if (pd && (!next || pd < next))
            next = pd;
        int timeout = -1;
        if (next) {
            double ms = (next - kr_now()) * 1000;
            timeout = ms < 0 ? 0 : ms > 60000 ? 60000 : (int)ms + 1;
        }
        /* Events X already read into its queue do not wake poll(). */
        if (m.screen && !kr_screen_dispatch(m.screen)) {
            stop(&m, 0, NULL);
            break;
        }
        if (poll(fds, (nfds_t)n, timeout) < 0) {
            if (errno == EINTR)
                continue;
            stop(&m, 1, strerror(errno));
            break;
        }
        if (fds[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si)
                stop(&m, 0, NULL);
        }
        if (v_at >= 0 && fds[v_at].revents)
            read_video(&m);
        kr_server_dispatch(m.server, fds + s_at, p_at - s_at);
        kr_procs_dispatch(m.procs, fds + p_at, n - p_at);
        kr_server_tick(m.server);
        if (x_at >= 0 && m.screen && fds[x_at].revents && !kr_screen_dispatch(m.screen))
            stop(&m, 0, NULL);
    }

    if (m.video_fd >= 0)
        close(m.video_fd); /* the server ends when its socket does */
    kr_server_free(m.server);
    kr_stream_free(m.stream);
    kr_decoder_free(m.dec);
    kr_screen_free(m.screen);
    kr_procs_free(m.procs);
    close(sfd);
    return m.status;
}
