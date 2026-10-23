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

#include "ctlmsg.h"
#include "decode.h"
#include "i18n.h"
#include "keys.h"
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
                   "  -n, --no-control      only show the screen, no mouse or keyboard\n"
                   "  -v, --verbose         debug messages (and the server's)\n"
                   "  -V, --version         show the version and exit\n"
                   "  -h, --help            show this help and exit\n"),
            "kiremote-mirror");
}

/* Motion is dropped rather than queued past this (the phone is slow to
 * read); clicks and keys never are. */
#define OUT_SOFT_LIMIT (256 * 1024)

typedef struct {
    kr_procs *procs;
    kr_server *server;
    int video_fd;
    int control_fd;
    uint8_t *out; /* control messages not written yet */
    size_t out_len, out_cap;
    kr_devmsg *devmsg;
    uint32_t buttons; /* Android buttons held (the left one is the touch) */
    uint32_t repeat;  /* of the key held */
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

/* ---- control -------------------------------------------------------------- */

static void flush_out(mirror *m)
{
    while (m->out_len) {
        ssize_t n = write(m->control_fd, m->out, m->out_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            stop(m, 1, strerror(errno));
            return;
        }
        memmove(m->out, m->out + n, m->out_len - (size_t)n);
        m->out_len -= (size_t)n;
    }
}

static void send_msg(mirror *m, const uint8_t *msg, size_t len, bool droppable)
{
    if (m->control_fd < 0 || m->done || (droppable && m->out_len > OUT_SOFT_LIMIT))
        return;
    if (m->out_len + len > m->out_cap) {
        size_t cap = m->out_cap ? m->out_cap * 2 : 4096;
        while (cap < m->out_len + len)
            cap *= 2;
        uint8_t *b = realloc(m->out, cap);
        if (!b)
            return;
        m->out = b;
        m->out_cap = cap;
    }
    memcpy(m->out + m->out_len, msg, len);
    m->out_len += len;
    flush_out(m);
}

static uint8_t msgbuf[KR_CTL_MAX_SIZE];

static void touch(mirror *m, int action, int x, int y, uint32_t action_button, bool droppable)
{
    size_t n = kr_ctl_touch(msgbuf, action, KR_POINTER_MOUSE, x, y, (uint16_t)m->vid_w, (uint16_t)m->vid_h,
                            action == KR_AMOTION_UP || action == KR_AMOTION_HOVER_MOVE ? 0.0f : 1.0f,
                            action_button, m->buttons);
    send_msg(m, msgbuf, n, droppable);
}

static void key(mirror *m, bool down, uint32_t keycode, uint32_t repeat, uint32_t meta)
{
    send_msg(m, msgbuf, kr_ctl_keycode(msgbuf, down ? KR_AKEY_DOWN : KR_AKEY_UP, keycode, repeat, meta), false);
}

/* Mouse buttons as scrcpy's defaults (cli.c): left touches, right is back
 * (or wakes the screen), middle is HOME, the side ones are recent apps and
 * the notifications; the wheel scrolls. */
static void on_button(void *ud, int button, bool down, int x, int y, unsigned state)
{
    mirror *m = ud;
    (void)state;
    switch (button) {
    case 1:
        if (down)
            m->buttons |= KR_ABUTTON_PRIMARY;
        else
            m->buttons &= ~(uint32_t)KR_ABUTTON_PRIMARY;
        touch(m, down ? KR_AMOTION_DOWN : KR_AMOTION_UP, x, y, KR_ABUTTON_PRIMARY, false);
        break;
    case 2:
        key(m, down, 3 /* AKEYCODE_HOME */, 0, 0);
        break;
    case 3:
        send_msg(m, msgbuf, kr_ctl_back_or_screen_on(msgbuf, down ? KR_AKEY_DOWN : KR_AKEY_UP), false);
        break;
    case 4: case 5: case 6: case 7:
        if (down) {
            float h = button == 6 ? -1.0f : button == 7 ? 1.0f : 0.0f;
            float v = button == 4 ? 1.0f : button == 5 ? -1.0f : 0.0f;
            size_t n = kr_ctl_scroll(msgbuf, x, y, (uint16_t)m->vid_w, (uint16_t)m->vid_h, h, v, m->buttons);
            send_msg(m, msgbuf, n, false);
        }
        break;
    case 8:
        key(m, down, 187 /* AKEYCODE_APP_SWITCH */, 0, 0);
        break;
    case 9:
        if (down)
            send_msg(m, msgbuf, kr_ctl_simple(msgbuf, KR_CTL_EXPAND_NOTIFICATIONS), false);
        break;
    default:
        break;
    }
}

static void on_motion(void *ud, int x, int y, unsigned state)
{
    mirror *m = ud;
    (void)state;
    /* Dragging moves the touch; otherwise the pointer hovers. */
    touch(m, (m->buttons & KR_ABUTTON_PRIMARY) ? KR_AMOTION_MOVE : KR_AMOTION_HOVER_MOVE, x, y, 0, true);
}

static void on_key(void *ud, unsigned long keysym, bool down, bool repeat, unsigned state, const char *text)
{
    mirror *m = ud;
    m->repeat = repeat ? m->repeat + 1 : 0;
    uint32_t code = kr_keys_keycode(keysym);
    if (code) {
        key(m, down, code, m->repeat, kr_keys_metastate(state));
        return;
    }
    /* What it typed, if anything printable (Ctrl+key types control codes). */
    if (!down || !text[0])
        return;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return;
    if (kr_utf8_valid(text, strlen(text)))
        send_msg(m, msgbuf, kr_ctl_text(msgbuf, text), false);
}

static void on_focus_out(void *ud)
{
    mirror *m = ud;
    /* A touch held when the window lost the pointer would stay down. */
    if (m->buttons & KR_ABUTTON_PRIMARY) {
        m->buttons &= ~(uint32_t)KR_ABUTTON_PRIMARY;
        touch(m, KR_AMOTION_UP, 0, 0, KR_ABUTTON_PRIMARY, false);
    }
}

static void read_control(mirror *m)
{
    uint8_t buf[16384];
    for (int round = 0; round < 8 && !m->done; round++) {
        ssize_t n = read(m->control_fd, buf, sizeof buf);
        if (n > 0) {
            if (!kr_devmsg_feed(m->devmsg, buf, (size_t)n))
                stop(m, 1, _("The phone sent a message that could not be read"));
            continue;
        }
        if (n == 0)
            stop(m, 0, _("The phone closed the connection"));
        else if (errno != EAGAIN && errno != EINTR)
            stop(m, 1, strerror(errno));
        return;
    }
}

/* ---- video ----------------------------------------------------------------- */

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
    if (!m->screen) {
        stop(m, 1, why);
        return;
    }
    if (m->control_fd >= 0) {
        static const kr_screen_input in = {on_button, on_motion, on_key, on_focus_out};
        kr_screen_set_input(m->screen, &in, m);
    }
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

static void server_ready(void *ud, int fd, int control_fd, const char *serial, const char *model)
{
    mirror *m = ud;
    if (control_fd >= 0) {
        fcntl(control_fd, F_SETFL, fcntl(control_fd, F_GETFL) | O_NONBLOCK);
        m->control_fd = control_fd;
        static const kr_devmsg_cbs dcbs = {NULL, NULL}; /* the clipboard comes later */
        m->devmsg = kr_devmsg_new(&dcbs, m);
    }
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
        {"no-control", no_argument, NULL, 'n'},
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    kr_server_opts opts = {0};
    int c;
    while ((c = getopt_long(argc, argv, "s:m:b:nvVh", longopts, NULL)) != -1) {
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
        case 'n':
            opts.view_only = true;
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

    mirror m = {.video_fd = -1, .control_fd = -1};
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
        int c_at = -1;
        if (m.control_fd >= 0) {
            c_at = n;
            fds[n++] = (struct pollfd){.fd = m.control_fd, .events = POLLIN | (m.out_len ? POLLOUT : 0)};
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
        if (c_at >= 0 && (fds[c_at].revents & POLLOUT))
            flush_out(&m);
        if (c_at >= 0 && (fds[c_at].revents & (POLLIN | POLLHUP | POLLERR)))
            read_control(&m);
        kr_server_dispatch(m.server, fds + s_at, p_at - s_at);
        kr_procs_dispatch(m.procs, fds + p_at, n - p_at);
        kr_server_tick(m.server);
        if (x_at >= 0 && m.screen && fds[x_at].revents && !kr_screen_dispatch(m.screen))
            stop(&m, 0, NULL);
    }

    if (m.video_fd >= 0)
        close(m.video_fd); /* the server ends when its socket does */
    if (m.control_fd >= 0)
        close(m.control_fd);
    kr_devmsg_free(m.devmsg);
    free(m.out);
    kr_server_free(m.server);
    kr_stream_free(m.stream);
    kr_decoder_free(m.dec);
    kr_screen_free(m.screen);
    kr_procs_free(m.procs);
    close(sfd);
    return m.status;
}
