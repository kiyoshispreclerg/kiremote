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

#include "adb.h"
#include "audio.h"
#include "camera.h"
#include "conf.h"
#include "config.h"
#include "ctlmsg.h"
#include "decode.h"
#include "i18n.h"
#include "keys.h"
#include "log.h"
#include "proc.h"
#include "screen.h"
#include "server.h"
#include "session.h"
#include "stream.h"
#include "util.h"
#include "v4l2.h"
#include "v4linfo.h"

static void usage(FILE *out)
{
    fprintf(out, _("Usage: %s [OPTION]...\n"
                   "Shows the phone's screen in a window (USB debugging must be on).\n"
                   "\n"
                   "  -s, --serial SERIAL   the phone, as adb names it (default: the only one)\n"
                   "  -m, --max-size N      longer side of the video, in pixels (default: the phone's)\n"
                   "  -b, --bit-rate N      video bit rate, in bits per second (8M, 500K...)\n"
                   "      --max-fps N       frames per second at most\n"
                   "      --codec CODEC     h264 (default), h265 or av1\n"
                   "  -n, --no-control      only show the screen, no mouse or keyboard\n"
                   "      --no-audio        do not play the phone's sound\n"
                   "      --no-clipboard    do not share the clipboard\n"
                   "      --connect ADDR    use the phone at ADDR[:PORT] over Wi-Fi (adb connect)\n"
                   "      --wifi ADDR       like --connect, but only when no phone is on a cable\n"
                   "      --tcpip           switch the phone on the cable to Wi-Fi and use that\n"
                   "      --id ID           the KDE Connect id of the phone: one window per phone\n"
                   "      --camera          the phone's camera as a webcam (v4l2loopback), no window\n"
                   "      --camera-facing F back (default), front or external\n"
                   "      --flip            flip the camera left to right, like a mirror\n"
                   "      --v4l2 DEVICE     the video device for the camera (default: the only v4l2loopback)\n"
                   "  -c, --config FILE     the settings file (default: ~/.config/kiremote/kiremote.ini)\n"
                   "  -v, --verbose         debug messages (and the server's)\n"
                   "  -V, --version         show the version and exit\n"
                   "  -h, --help            show this help and exit\n"
                   "\n"
                   "In the window, Alt+key (or Super+key) acts on the phone: h home, b back,\n"
                   "s recent apps, m menu, p power, Up and Down volume, n notifications (Shift:\n"
                   "close them), r rotate, o screen off (Shift: on); and f fullscreen.\n"),
            "kiremote-mirror");
}

/* Motion is dropped rather than queued past this (the phone is slow to
 * read); clicks and keys never are. */
#define OUT_SOFT_LIMIT (256 * 1024)

typedef struct {
    kr_procs *procs;
    kr_server *server;
    int video_fd;
    int audio_fd;
    int control_fd;
    uint8_t *out; /* control messages not written yet */
    size_t out_len, out_cap;
    kr_devmsg *devmsg;
    uint32_t buttons; /* Android buttons held (the left one is the touch) */
    uint32_t repeat;  /* of the key held */
    unsigned long swallowed; /* the last key a shortcut took, until it is released */
    kr_stream *stream;
    kr_stream *astream;
    kr_audio *audio;
    kr_decoder *dec;
    kr_screen *screen;
    kr_inst *inst;
    bool camera; /* the camera to a video device, not the screen to a window */
    bool flip;
    const char *v4l_path;
    kr_camera *cam;
    const char *mode; /* "mirror" */
    char name[KR_STREAM_NAME_SIZE];
    char model[64];
    unsigned vid_w, vid_h;
    bool clipboard; /* shared with the computer's */
    char *clip_last; /* the text last moved either way: what comes back is not news */
    size_t clip_last_len;
    uint64_t clip_seq;
    bool screen_off;
    bool done;
    int status; /* exit status */
    /* Over Wi-Fi, a lost connection is retried instead of ending (lost()). */
    kr_server_opts opts; /* what to start the server with again */
    char wifi_serial[128]; /* the phone's adb serial if it is an address (HOST:PORT), else "" */
    char title[128];
    bool ready_once;
    bool down;        /* the connection is gone and we are getting it back */
    bool drop_server; /* free m->server at the end of this turn of the loop */
    bool reconnected; /* the screen is up from before: say the phone's screen is off again */
    double down_since, retry_at;
    int attempts;
} mirror;

/* How long a lost Wi-Fi connection is tried again before giving up, and the
 * wait between tries (grows to the maximum). */
#define RECONNECT_FOR_S 120
#define RECONNECT_MAX_WAIT_S 10

static void stop(mirror *m, int status, const char *msg)
{
    if (msg)
        kr_log_error("%s", msg);
    if (!m->done)
        m->status = status;
    m->done = true;
}

static void lost(mirror *m, int status, const char *msg);

/* ---- control -------------------------------------------------------------- */

static void flush_out(mirror *m)
{
    while (m->out_len && m->control_fd >= 0) {
        ssize_t n = write(m->control_fd, m->out, m->out_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            lost(m, 1, strerror(errno));
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

/* A tap of an Android key. */
static void tap(mirror *m, uint32_t keycode)
{
    key(m, true, keycode, 0, 0);
    key(m, false, keycode, 0, 0);
}

/* The Alt+key shortcuts (keys.h). down only: a release does nothing. */
static void shortcut(mirror *m, kr_shortcut sc, bool repeat)
{
    switch (sc) {
    case KR_SC_HOME:
        tap(m, 3);
        break;
    case KR_SC_BACK:
        send_msg(m, msgbuf, kr_ctl_back_or_screen_on(msgbuf, KR_AKEY_DOWN), false);
        send_msg(m, msgbuf, kr_ctl_back_or_screen_on(msgbuf, KR_AKEY_UP), false);
        break;
    case KR_SC_APP_SWITCH:
        tap(m, 187);
        break;
    case KR_SC_MENU:
        tap(m, 82);
        break;
    case KR_SC_POWER:
        tap(m, 26);
        break;
    case KR_SC_VOLUME_UP:
        tap(m, 24); /* held, it goes on */
        break;
    case KR_SC_VOLUME_DOWN:
        tap(m, 25);
        break;
    case KR_SC_NOTIFICATIONS:
        send_msg(m, msgbuf, kr_ctl_simple(msgbuf, KR_CTL_EXPAND_NOTIFICATIONS), false);
        break;
    case KR_SC_COLLAPSE:
        send_msg(m, msgbuf, kr_ctl_simple(msgbuf, KR_CTL_COLLAPSE_PANELS), false);
        break;
    case KR_SC_ROTATE:
        if (!repeat)
            send_msg(m, msgbuf, kr_ctl_simple(msgbuf, KR_CTL_ROTATE_DEVICE), false);
        break;
    case KR_SC_SCREEN_OFF:
    case KR_SC_SCREEN_ON:
        if (!repeat)
            send_msg(m, msgbuf, kr_ctl_set_display_power(msgbuf, sc == KR_SC_SCREEN_ON), false);
        break;
    case KR_SC_FULLSCREEN:
        if (!repeat)
            kr_screen_toggle_fullscreen(m->screen);
        break;
    case KR_SC_NONE:
        break;
    }
}

static void on_key(void *ud, unsigned long keysym, bool down, bool repeat, unsigned state, const char *text)
{
    mirror *m = ud;
    kr_shortcut sc = kr_keys_shortcut(keysym, state);
    if (sc != KR_SC_NONE) {
        m->swallowed = keysym;
        if (down && (!repeat || sc == KR_SC_VOLUME_UP || sc == KR_SC_VOLUME_DOWN))
            shortcut(m, sc, repeat);
        return;
    }
    if (!down && m->swallowed && m->swallowed == keysym) { /* the release of a shortcut, Alt already up or not */
        m->swallowed = 0;
        return;
    }
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
    for (int round = 0; round < 8 && !m->done && m->control_fd >= 0; round++) {
        ssize_t n = read(m->control_fd, buf, sizeof buf);
        if (n > 0) {
            if (!kr_devmsg_feed(m->devmsg, buf, (size_t)n))
                stop(m, 1, _("The phone sent a message that could not be read"));
            continue;
        }
        if (n == 0)
            lost(m, 0, _("The phone closed the connection"));
        else if (errno != EAGAIN && errno != EINTR)
            lost(m, 1, strerror(errno));
        return;
    }
}

/* ---- clipboard --------------------------------------------------------------- */

static bool clip_is_last(const mirror *m, const char *text, size_t len)
{
    return m->clip_last && m->clip_last_len == len && memcmp(m->clip_last, text, len) == 0;
}

static void clip_remember(mirror *m, const char *text, size_t len)
{
    char *copy = malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, text, len);
    copy[len] = 0;
    free(m->clip_last);
    m->clip_last = copy;
    m->clip_last_len = len;
}

/* The phone copied something: it is the computer's clipboard now. */
static void on_phone_clipboard(void *ud, const char *text, size_t len)
{
    mirror *m = ud;
    if (!m->clipboard || !m->screen || !len || clip_is_last(m, text, len) || !kr_utf8_valid(text, len)) {
        kr_log_debug("clipboard: phone's %zu bytes ignored", len);
        return;
    }
    kr_log_debug("clipboard: phone -> computer, %zu bytes", len);
    clip_remember(m, text, len);
    kr_screen_clipboard_set(m->screen, text, len);
}

/* What the computer's holder said when asked. */
static void on_computer_clipboard(void *ud, const char *text, size_t len)
{
    mirror *m = ud;
    if (!text || !len || len > KR_CTL_CLIPBOARD_MAX || clip_is_last(m, text, len) || !kr_utf8_valid(text, len)) {
        kr_log_debug("clipboard: computer's text not sent (%zu bytes)", text ? len : 0);
        return;
    }
    kr_log_debug("clipboard: computer -> phone, %zu bytes", len);
    clip_remember(m, text, len);
    send_msg(m, msgbuf, kr_ctl_set_clipboard(msgbuf, ++m->clip_seq, m->clip_last, false), false);
}

/* Somebody copied on the computer (or the window got the keyboard). */
static void on_computer_changed(void *ud)
{
    mirror *m = ud;
    kr_screen_clipboard_get(m->screen, on_computer_clipboard, m);
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
    if (m->camera) {
        if (m->cam)
            return; /* the device keeps its size; frames are scaled to it */
        const char *why = NULL;
        unsigned cw = w & ~1u, ch = h & ~1u; /* YU12 needs even sides */
        kr_v4l2 *out = kr_v4l2_open(m->v4l_path, cw, ch, &why);
        m->cam = out ? kr_camera_new(out, cw, ch, m->flip) : NULL;
        if (!m->cam) {
            if (out)
                kr_v4l2_close(out);
            stop(m, 1, why ? why : _("Out of memory"));
        } else {
            kr_log_info(_("The camera is %s now (%ux%u)"), m->v4l_path[0] ? m->v4l_path : "(a file)", cw, ch);
        }
        return;
    }
    if (m->screen) {
        kr_screen_video_size(m->screen, w, h);
        return;
    }
    snprintf(m->title, sizeof m->title, "%s", m->name[0] ? m->name : m->model[0] ? m->model : "Android");
    const char *why = NULL;
    m->screen = kr_screen_new(m->title, w, h, &why);
    if (!m->screen) {
        stop(m, 1, why);
        return;
    }
    if (m->control_fd >= 0) {
        static const kr_screen_input in = {on_button, on_motion, on_key, on_focus_out};
        kr_screen_set_input(m->screen, &in, m);
        if (m->clipboard)
            kr_screen_clipboard_watch(m->screen, on_computer_changed, m);
        if (m->screen_off) /* the phone's own screen; the picture goes on */
            send_msg(m, msgbuf, kr_ctl_set_display_power(msgbuf, false), false);
    }
}

static void on_frame(void *ud, const AVFrame *f)
{
    mirror *m = ud;
    if (m->cam && !kr_camera_frame(m->cam, f))
        stop(m, 1, _("The video device went away"));
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

/* ---- audio ----------------------------------------------------------------- */

/* The sound ends here, the picture goes on. */
static void drop_audio(mirror *m, const char *why)
{
    if (why)
        kr_log_warn("%s", why);
    if (m->audio_fd >= 0)
        close(m->audio_fd);
    m->audio_fd = -1;
    kr_stream_free(m->astream);
    m->astream = NULL;
    kr_audio_free(m->audio);
    m->audio = NULL;
}

/* The connection to the phone is gone. Over the network (the phone's serial is
 * an address) that is often only the Wi-Fi: the window, the camera device and
 * the claim on the phone stay, the server is started again (main loop) with
 * `adb connect` first, for RECONNECT_FOR_S seconds. Over a cable, or before
 * it ever worked, it is the end, as it was. */
static void lost(mirror *m, int status, const char *msg)
{
    if (m->done || m->down)
        return;
    if (!m->ready_once || !m->wifi_serial[0]) {
        stop(m, status, msg);
        return;
    }
    kr_log_warn(_("Lost the connection to the phone (%s); trying to get it back"), msg);
    if (m->video_fd >= 0)
        close(m->video_fd);
    if (m->control_fd >= 0)
        close(m->control_fd);
    m->video_fd = m->control_fd = -1;
    drop_audio(m, NULL);
    kr_stream_free(m->stream);
    m->stream = NULL;
    kr_devmsg_free(m->devmsg);
    m->devmsg = NULL;
    kr_decoder_free(m->dec); /* the next stream says its codec again */
    m->dec = NULL;
    m->out_len = 0;
    m->buttons = 0;
    m->down = true;
    m->drop_server = true;
    m->down_since = kr_now();
    m->attempts = 0;
    m->retry_at = m->down_since + 1;
    if (m->screen) {
        char t[sizeof m->title + 32];
        snprintf(t, sizeof t, _("%s (reconnecting...)"), m->title);
        kr_screen_set_title(m->screen, t);
    }
}

static void on_audio_codec(void *ud, uint32_t codec)
{
    mirror *m = ud;
    kr_log_debug("audio codec %08x", codec);
    if (codec != KR_CODEC_RAW)
        drop_audio(m, _("The phone sent sound in a format this program does not play"));
}

static void on_audio_packet(void *ud, const uint8_t *data, size_t len, int64_t pts, bool key)
{
    mirror *m = ud;
    (void)pts;
    (void)key;
    static size_t total;
    static int n;
    total += len;
    static int peak;
    for (size_t i = 0; i + 1 < len; i += 2) {
        int v = (int16_t)(data[i] | data[i + 1] << 8);
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
    }
    if (n++ % 25 == 0) {
        kr_log_debug("audio: %d packets, %zu bytes, queued %zu, peak %d", n, total,
                     m->audio ? kr_audio_queued(m->audio) : 0, peak);
        peak = 0;
    }
    if (m->audio)
        kr_audio_play(m->audio, data, len);
}

static void read_audio(mirror *m)
{
    static uint8_t buf[64 * 1024];
    for (int round = 0; round < 8 && m->audio_fd >= 0; round++) {
        ssize_t n = read(m->audio_fd, buf, sizeof buf);
        if (n > 0) {
            if (!kr_stream_feed(m->astream, buf, (size_t)n)) {
                /* Before Android 11 the phone says "no sound" and the socket just ends. */
                drop_audio(m, kr_stream_disabled(m->astream) ? NULL : kr_stream_error(m->astream));
                return;
            }
            if (kr_stream_disabled(m->astream)) {
                drop_audio(m, _("The phone cannot share its sound (Android 11 or newer is needed)"));
                return;
            }
            continue;
        }
        if (n == 0)
            drop_audio(m, NULL);
        else if (errno != EAGAIN && errno != EINTR)
            drop_audio(m, strerror(errno));
        return;
    }
}

static void start_audio(mirror *m, int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    m->audio_fd = fd;
    static const kr_stream_cbs cbs = {NULL, on_audio_codec, NULL, on_audio_packet};
    m->astream = kr_stream_new(false, false, &cbs, m);
    const char *why = NULL;
    m->audio = m->astream ? kr_audio_new("Kiremote", m->model[0] ? m->model : "Android", &why) : NULL;
    if (!m->audio)
        drop_audio(m, why ? why : _("Out of memory"));
}

/* ---- one per phone ---------------------------------------------------------- */

static void on_raise(void *ud)
{
    mirror *m = ud;
    if (m->screen)
        kr_screen_raise(m->screen);
}

static void on_quit(void *ud)
{
    stop(ud, 0, NULL);
}

/* Takes the phone (session.h). false if another process has it: it is told to
 * come forward, or the user is told what it is busy with. */
static bool claim_phone(mirror *m, const char *key)
{
    static const kr_inst_cbs cbs = {on_raise, on_quit};
    char other[16];
    m->inst = kr_inst_claim(key, m->mode, &cbs, m, other, sizeof other);
    if (m->inst || !other[0])
        return true; /* ours, or no way to tell: go on */
    if (!strcmp(other, m->mode)) {
        kr_inst_send(key, "raise");
        kr_log_info(_("Already open for this phone"));
        stop(m, 0, NULL);
    } else if (!strcmp(other, "camera")) {
        stop(m, 1, _("This phone is being used as a webcam: stop that first"));
    } else {
        stop(m, 1, _("This phone's screen is already being mirrored: close that window first"));
    }
    return false;
}

static void server_ready(void *ud, int fd, int audio_fd, int control_fd, const char *serial, const char *model)
{
    mirror *m = ud;
    if (!m->inst && !m->done && !claim_phone(m, serial)) {
        close(fd);
        if (audio_fd >= 0)
            close(audio_fd);
        if (control_fd >= 0)
            close(control_fd);
        return;
    }
    if (control_fd >= 0) {
        fcntl(control_fd, F_SETFL, fcntl(control_fd, F_GETFL) | O_NONBLOCK);
        m->control_fd = control_fd;
        static const kr_devmsg_cbs dcbs = {on_phone_clipboard, NULL};
        m->devmsg = kr_devmsg_new(&dcbs, m);
    }
    if (m->down) {
        kr_log_info(_("Got the phone back"));
        m->down = false;
        m->reconnected = true;
        if (m->screen) {
            kr_screen_set_title(m->screen, m->title);
            if (m->screen_off && control_fd >= 0) /* the new server starts with the phone's screen on */
                send_msg(m, msgbuf, kr_ctl_set_display_power(msgbuf, false), false);
        }
    } else {
        kr_log_info(m->camera ? _("Using the camera of %s") : _("Mirroring %s"), model && *model ? model : serial);
    }
    m->ready_once = true;
    m->attempts = 0;
    snprintf(m->wifi_serial, sizeof m->wifi_serial, "%s", serial && kr_adb_is_tcpip(serial) ? serial : "");
    snprintf(m->model, sizeof m->model, "%s", model ? model : "");
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    m->video_fd = fd;
    if (audio_fd >= 0)
        start_audio(m, audio_fd);
    static const kr_stream_cbs cbs = {on_name, on_codec, on_session, on_packet};
    m->stream = kr_stream_new(true, true, &cbs, m);
    if (!m->stream)
        stop(m, 1, _("Out of memory"));
}

static void server_error(void *ud, const char *message)
{
    mirror *m = ud;
    if (m->drop_server) /* this server was given up already; its last words are no news */
        return;
    if (m->down && !m->done) { /* a try to get the phone back failed: later, or never */
        double now = kr_now();
        kr_log_debug("reconnect: %s", message);
        m->drop_server = true;
        if (now - m->down_since > RECONNECT_FOR_S) {
            stop(m, 1, _("The phone did not come back"));
            return;
        }
        double wait = 2.0 * ++m->attempts;
        m->retry_at = now + (wait > RECONNECT_MAX_WAIT_S ? RECONNECT_MAX_WAIT_S : wait);
        return;
    }
    lost(m, 1, message);
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
    for (int round = 0; round < 8 && !m->done && m->video_fd >= 0; round++) {
        ssize_t n = read(m->video_fd, buf, sizeof buf);
        if (n > 0) {
            if (!kr_stream_feed(m->stream, buf, (size_t)n))
                stop(m, 1, kr_stream_error(m->stream));
            continue;
        }
        if (n == 0)
            lost(m, 0, _("The phone closed the connection"));
        else if (errno != EAGAIN && errno != EINTR)
            lost(m, 1, strerror(errno));
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

    enum { OPT_MAX_FPS = 256, OPT_CODEC, OPT_NO_AUDIO, OPT_NO_CLIPBOARD, OPT_CONNECT, OPT_WIFI, OPT_TCPIP, OPT_ID, OPT_CAMERA, OPT_FACING, OPT_FLIP, OPT_V4L2 };
    static const struct option longopts[] = {
        {"serial", required_argument, NULL, 's'},
        {"max-size", required_argument, NULL, 'm'},
        {"bit-rate", required_argument, NULL, 'b'},
        {"max-fps", required_argument, NULL, OPT_MAX_FPS},
        {"codec", required_argument, NULL, OPT_CODEC},
        {"no-control", no_argument, NULL, 'n'},
        {"no-audio", no_argument, NULL, OPT_NO_AUDIO},
        {"no-clipboard", no_argument, NULL, OPT_NO_CLIPBOARD},
        {"connect", required_argument, NULL, OPT_CONNECT},
        {"wifi", required_argument, NULL, OPT_WIFI},
        {"tcpip", no_argument, NULL, OPT_TCPIP},
        {"id", required_argument, NULL, OPT_ID},
        {"camera", no_argument, NULL, OPT_CAMERA},
        {"camera-facing", required_argument, NULL, OPT_FACING},
        {"flip", no_argument, NULL, OPT_FLIP},
        {"v4l2", required_argument, NULL, OPT_V4L2},
        {"config", required_argument, NULL, 'c'},
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    /* The settings file first, then the command line over it. */
    const char *config_path = NULL;
    int c;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "-c:", longopts, NULL)) != -1)
        if (c == 'c')
            config_path = optarg;
    opterr = 1;
    optind = 0; /* start over */
    kr_config *cfg = kr_config_load(config_path);
    kr_mirror_conf conf;
    kr_mirror_conf_load(&conf, cfg);
    kr_config_free(cfg);

    kr_server_opts opts = {0};
    const char *id = NULL;
    bool camera = false;
    while ((c = getopt_long(argc, argv, "s:m:b:nvVhc:", longopts, NULL)) != -1) {
        bool ok = true;
        switch (c) {
        case 's':
            opts.serial = optarg;
            break;
        case 'm':
            ok = parse_uint(optarg, &conf.max_size) && conf.max_size <= KR_STREAM_MAX_SIDE;
            break;
        case 'b':
            ok = kr_parse_bit_rate(optarg, &conf.bit_rate);
            break;
        case OPT_MAX_FPS:
            ok = parse_uint(optarg, &conf.max_fps);
            break;
        case OPT_CODEC:
            ok = !strcmp(optarg, "h264") || !strcmp(optarg, "h265") || !strcmp(optarg, "av1");
            if (ok)
                snprintf(conf.codec, sizeof conf.codec, "%s", optarg);
            break;
        case 'n':
            conf.control = false;
            break;
        case OPT_NO_AUDIO:
            conf.audio = false;
            break;
        case OPT_NO_CLIPBOARD:
            conf.clipboard = false;
            break;
        case OPT_CONNECT:
            opts.connect = optarg;
            opts.connect_fallback = false;
            break;
        case OPT_WIFI:
            if (conf.wifi && !(opts.connect && !opts.connect_fallback)) { /* the settings can turn it off */
                opts.connect = optarg;
                opts.connect_fallback = true;
            }
            break;
        case OPT_TCPIP:
            opts.tcpip = true;
            break;
        case OPT_ID:
            id = optarg;
            break;
        case OPT_CAMERA:
            camera = true;
            break;
        case OPT_FACING:
            ok = !strcmp(optarg, "back") || !strcmp(optarg, "front") || !strcmp(optarg, "external");
            if (ok)
                snprintf(conf.camera_facing, sizeof conf.camera_facing, "%s", optarg);
            break;
        case OPT_FLIP:
            conf.camera_flip = true;
            break;
        case OPT_V4L2:
            ok = optarg[0] == '/' && strlen(optarg) < sizeof conf.camera_device;
            if (ok)
                snprintf(conf.camera_device, sizeof conf.camera_device, "%s", optarg);
            break;
        case 'c':
            break; /* read above */
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
    opts.max_size = conf.max_size;
    opts.bit_rate = conf.bit_rate;
    opts.max_fps = conf.max_fps;
    opts.codec = conf.codec;
    opts.view_only = !conf.control;
    opts.audio = conf.audio;
    opts.stay_awake = conf.stay_awake;
    if (optind < argc) {
        usage(stderr);
        return 2;
    }
    char v4l_path[96] = "";
    if (camera) { /* no window, no control, no sound: only the picture, to the video device */
        const char *why = NULL;
        kr_v4l v4l;
        kr_v4l_scan(&v4l);
        if (!kr_v4l_pick(&v4l, conf.camera_device, v4l_path, sizeof v4l_path, &why) && !getenv("KIREMOTE_V4L2_FILE")) {
            kr_log_error("%s", why);
            return 1;
        }
        opts.camera = true;
        opts.camera_facing = conf.camera_facing;
        if (!opts.max_size)
            opts.max_size = conf.camera_max_size;
        opts.view_only = true;
        opts.audio = false;
        opts.stay_awake = false;
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

    mirror m = {.v4l_path = v4l_path, .mode = camera ? "camera" : "mirror", .camera = camera, .flip = conf.camera_flip, .video_fd = -1, .audio_fd = -1, .control_fd = -1, .clipboard = conf.clipboard, .screen_off = conf.screen_off};
    if (id && *id)
        claim_phone(&m, id); /* before anything is copied to the phone */
    m.opts = opts;
    m.procs = kr_procs_new();
    static const kr_server_cb scb = {server_ready, server_error, server_log};
    if (m.procs && !m.done)
        m.server = kr_server_start(m.procs, &opts, &scb, &m);
    if (!m.server && !m.done)
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
        int i_at = n, i_n = 0;
        if (m.inst)
            n += i_n = kr_inst_pollfds(m.inst, fds + n, 8);
        int a_at = -1;
        if (m.audio_fd >= 0) {
            a_at = n;
            fds[n++] = (struct pollfd){.fd = m.audio_fd, .events = POLLIN};
        }
        int c_at = -1;
        if (m.control_fd >= 0) {
            c_at = n;
            fds[n++] = (struct pollfd){.fd = m.control_fd, .events = POLLIN | (m.out_len ? POLLOUT : 0)};
        }
        int s_at = n;
        if (m.server)
            n += kr_server_pollfds(m.server, fds + n, MAX_FDS - n);
        int p_at = n;
        n += kr_procs_pollfds(m.procs, fds + n, MAX_FDS - n);

        double next = m.server ? kr_server_deadline(m.server) : 0, pd = kr_procs_deadline(m.procs);
        if (pd && (!next || pd < next))
            next = pd;
        if (m.down && !m.server && (!next || m.retry_at < next))
            next = m.retry_at;
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
        if ((m.audio ? kr_audio_poll(m.audio, fds, (nfds_t)n, timeout) : poll(fds, (nfds_t)n, timeout)) < 0) {
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
        if (m.inst)
            kr_inst_dispatch(m.inst, fds + i_at, i_n);
        if (a_at >= 0 && fds[a_at].revents)
            read_audio(&m);
        if (m.audio && !kr_audio_ok(m.audio))
            drop_audio(&m, _("The sound stopped"));
        if (c_at >= 0 && (fds[c_at].revents & POLLOUT))
            flush_out(&m);
        if (c_at >= 0 && (fds[c_at].revents & (POLLIN | POLLHUP | POLLERR)))
            read_control(&m);
        if (m.server)
            kr_server_dispatch(m.server, fds + s_at, p_at - s_at);
        kr_procs_dispatch(m.procs, fds + p_at, n - p_at);
        if (m.server)
            kr_server_tick(m.server);
        if (m.drop_server) { /* not from inside its own callbacks */
            kr_server_free(m.server);
            m.server = NULL;
            m.drop_server = false;
        }
        if (m.down && !m.server && !m.done && kr_now() >= m.retry_at) {
            kr_server_opts again = m.opts;
            again.serial = again.connect = m.wifi_serial; /* adb connect it, then that phone only */
            again.connect_fallback = false;
            again.tcpip = false;
            m.server = kr_server_start(m.procs, &again, &scb, &m);
            if (!m.server)
                stop(&m, 1, _("Out of memory"));
        }
        if (x_at >= 0 && m.screen && fds[x_at].revents && !kr_screen_dispatch(m.screen))
            stop(&m, 0, NULL);
    }

    if (m.video_fd >= 0)
        close(m.video_fd); /* the server ends when its socket does */
    if (m.control_fd >= 0)
        close(m.control_fd);
    drop_audio(&m, NULL);
    kr_inst_free(m.inst);
    kr_devmsg_free(m.devmsg);
    free(m.clip_last);
    free(m.out);
    kr_server_free(m.server);
    kr_stream_free(m.stream);
    kr_decoder_free(m.dec);
    kr_screen_free(m.screen);
    kr_camera_free(m.cam);
    kr_procs_free(m.procs);
    close(sfd);
    return m.status;
}
