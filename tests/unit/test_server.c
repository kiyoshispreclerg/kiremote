/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Starting the scrcpy-server through adb, against tests/tools/fake_adb: the
 * commands in order, the socket handed over with the phone's stream on it,
 * and each way it can fail. */
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "server.h"
#include "stream.h"
#include "util.h"

#define TOOLS KR_FIXTURES_DIR "/../tools"

typedef struct {
    bool ready, failed;
    int fd, audio_fd, control_fd;
    char serial[64], model[64];
    char error[512];
    int log_lines;
} result;

static void on_ready(void *ud, int fd, int audio_fd, int control_fd, const char *serial, const char *model)
{
    result *r = ud;
    r->ready = true;
    r->fd = fd;
    r->audio_fd = audio_fd;
    r->control_fd = control_fd;
    snprintf(r->serial, sizeof r->serial, "%s", serial);
    snprintf(r->model, sizeof r->model, "%s", model);
}

static void on_error(void *ud, const char *message)
{
    result *r = ud;
    r->failed = true;
    snprintf(r->error, sizeof r->error, "%s", message);
}

static void on_log(void *ud, const char *line)
{
    (void)line;
    ((result *)ud)->log_lines++;
}

static char dir[64], log_path[128];

static void setup(const char *mode)
{
    snprintf(dir, sizeof dir, "/tmp/kiremote-server-XXXXXX");
    if (!mkdtemp(dir))
        abort();
    snprintf(log_path, sizeof log_path, "%s/adb.log", dir);
    setenv("KIREMOTE_ADB", TOOLS "/fake_adb", 1);
    setenv("FAKEADB_LOG", log_path, 1);
    setenv("FAKEADB_STREAM", KR_FIXTURES_DIR "/scrcpy-video.bin", 1);
    setenv("FAKEADB_MODE", mode, 1);
}

static void teardown(void)
{
    char p[160];
    static const char *const extras[] = {"port", "connected", "tcpip", "connects"};
    for (size_t i = 0; i < sizeof extras / sizeof *extras; i++) {
        snprintf(p, sizeof p, "%s/adb.log.%s", dir, extras[i]);
        unlink(p);
    }
    unlink(log_path);
    rmdir(dir);
}

/* Runs the loop until it is ready or failed (or 10 s). */
static void run(kr_procs *ps, kr_server *s, result *r)
{
    double end = kr_now() + 10;
    while (!r->ready && !r->failed && kr_now() < end) {
        struct pollfd fds[70];
        int n = kr_server_pollfds(s, fds, 70);
        int ns = n;
        n += kr_procs_pollfds(ps, fds + n, 70 - n);
        poll(fds, (nfds_t)n, 50);
        kr_server_dispatch(s, fds, ns);
        kr_procs_dispatch(ps, fds + ns, n - ns);
        kr_server_tick(s);
    }
}

typedef struct {
    char name[KR_STREAM_NAME_SIZE];
    unsigned w, h;
} got;

static void g_name(void *ud, const char *name)
{
    snprintf(((got *)ud)->name, KR_STREAM_NAME_SIZE, "%s", name);
}

static void g_session(void *ud, uint32_t w, uint32_t h, bool resized)
{
    (void)resized;
    ((got *)ud)->w = w;
    ((got *)ud)->h = h;
}

TEST(the_server_starts_and_its_socket_is_handed_over)
{
    setup("");
    kr_procs *ps = kr_procs_new();
    result r = {.fd = -1};
    kr_server_opts o = {.jar = KR_FIXTURES_DIR "/scrcpy-video.bin", .max_size = 800};
    static const kr_server_cb cb = {on_ready, on_error, on_log};
    kr_server *s = kr_server_start(ps, &o, &cb, &r);
    run(ps, s, &r);
    CHECK_MSG(r.ready, "not ready: %s", r.error);
    CHECK_STR_EQ(r.serial, "FAKE123");
    CHECK_STR_EQ(r.model, "Fake_Phone");

    /* The phone's stream comes through. */
    got g = {0};
    static const kr_stream_cbs scb = {g_name, NULL, g_session, NULL};
    kr_stream *st = kr_stream_new(true, true, &scb, &g);
    uint8_t buf[4096];
    double end = kr_now() + 5;
    while (!g.w && kr_now() < end) {
        ssize_t n = read(r.fd, buf, sizeof buf);
        if (n <= 0)
            break;
        CHECK(kr_stream_feed(st, buf, (size_t)n));
    }
    kr_stream_free(st);
    CHECK_STR_EQ(g.name, "2201117TL");
    CHECK_INT_EQ(g.w, 360);

    /* Let the tunnel removal run, then look at what adb was asked. */
    for (int i = 0; i < 40; i++) {
        struct pollfd fds[64];
        int n = kr_procs_pollfds(ps, fds, 64);
        poll(fds, (nfds_t)n, 25);
        kr_procs_dispatch(ps, fds, n);
    }
    size_t len;
    char *log = kr_read_file(log_path, &len);
    CHECK(log);
    CHECK_STR_HAS(log, "devices -l\n");
    CHECK_STR_HAS(log, "-s FAKE123 push " KR_FIXTURES_DIR "/scrcpy-video.bin /data/local/tmp/kiremote-scrcpy-server.jar\n");
    char *rev = strstr(log, "-s FAKE123 reverse localabstract:scrcpy_");
    CHECK(rev);
    char scid[9];
    memcpy(scid, rev + strlen("-s FAKE123 reverse localabstract:scrcpy_"), 8);
    scid[8] = 0;
    CHECK(strspn(scid, "0123456789abcdef") == 8 && scid[0] <= '7'); /* 31 bits, %08x */
    char want[256];
    snprintf(want, sizeof want,
             "-s FAKE123 shell CLASSPATH=/data/local/tmp/kiremote-scrcpy-server.jar app_process / "
             "com.genymobile.scrcpy.Server " KR_SCRCPY_VERSION " scid=%s log_level=info audio=false "
             "video_codec=h264 max_size=800\n",
             scid);
    CHECK_STR_HAS(log, want);
    snprintf(want, sizeof want, "-s FAKE123 reverse --remove localabstract:scrcpy_%s\n", scid);
    CHECK_STR_HAS(log, want);
    CHECK(r.log_lines >= 1);
    free(log);

    /* The control socket is the second one the server opened. */
    CHECK(r.control_fd >= 0);
    CHECK(write(r.control_fd, "\x04\x00", 2) == 2); /* back, down */
    char echo[2];
    CHECK(read(r.control_fd, echo, 2) == 2 && echo[0] == 4);
    close(r.control_fd);
    close(r.fd);
    kr_server_free(s);
    kr_procs_free(ps);
    teardown();
}

static void expect_failure(const char *mode, double timeout, const char *needle1, const char *needle2)
{
    setup(mode);
    kr_procs *ps = kr_procs_new();
    result r = {.fd = -1};
    kr_server_opts o = {.jar = KR_FIXTURES_DIR "/scrcpy-video.bin", .timeout = timeout};
    static const kr_server_cb cb = {on_ready, on_error, on_log};
    kr_server *s = kr_server_start(ps, &o, &cb, &r);
    run(ps, s, &r);
    kr_server_free(s);
    kr_procs_free(ps);
    teardown();
    CHECK(!r.ready);
    CHECK_MSG(r.failed, "%s: no error", mode);
    CHECK_STR_HAS(r.error, needle1);
    if (needle2)
        CHECK_STR_HAS(r.error, needle2);
}

TEST(audio_is_a_socket_between_video_and_control)
{
    setup("");
    kr_procs *ps = kr_procs_new();
    result r = {.fd = -1};
    kr_server_opts o = {.jar = KR_FIXTURES_DIR "/scrcpy-video.bin", .audio = true, .stay_awake = true};
    static const kr_server_cb cb = {on_ready, on_error, on_log};
    kr_server *s = kr_server_start(ps, &o, &cb, &r);
    run(ps, s, &r);
    CHECK_MSG(r.ready, "not ready: %s", r.error);
    CHECK(r.fd >= 0 && r.audio_fd >= 0 && r.control_fd >= 0);
    CHECK(r.fd != r.audio_fd && r.audio_fd != r.control_fd);
    /* The echo comes from the control socket, the last one. */
    CHECK(write(r.control_fd, "\x04\x00", 2) == 2);
    char echo[2];
    CHECK(read(r.control_fd, echo, 2) == 2 && echo[0] == 4);
    close(r.control_fd);
    close(r.audio_fd);
    close(r.fd);
    kr_server_free(s);
    kr_procs_free(ps);
    size_t len;
    char *log = kr_read_file(log_path, &len);
    CHECK(log);
    CHECK_STR_HAS(log, "audio=true video_codec=h264");
    CHECK_STR_HAS(log, "audio_codec=raw");
    CHECK_STR_HAS(log, "stay_awake=true");
    free(log);
    teardown();
}

/* Runs a server with opts to the end; the adb log is in *log (caller frees). */
static result over_the_network(const char *mode, kr_server_opts o, char **log)
{
    setup(mode);
    kr_procs *ps = kr_procs_new();
    result r = {.fd = -1, .audio_fd = -1, .control_fd = -1};
    o.jar = KR_FIXTURES_DIR "/scrcpy-video.bin";
    o.view_only = true;
    o.timeout = 5;
    static const kr_server_cb cb = {on_ready, on_error, on_log};
    kr_server *s = kr_server_start(ps, &o, &cb, &r);
    run(ps, s, &r);
    if (r.fd >= 0)
        close(r.fd);
    kr_server_free(s);
    kr_procs_free(ps);
    size_t len;
    *log = kr_read_file(log_path, &len);
    teardown();
    return r;
}

TEST(a_phone_is_connected_by_address)
{
    char *log;
    kr_server_opts o = {.connect = "192.168.1.42"};
    result r = over_the_network("nousb", o, &log);
    CHECK_MSG(r.ready, "not ready: %s", r.error);
    CHECK_STR_EQ(r.serial, "192.168.1.42:5555");
    CHECK(log);
    CHECK_STR_HAS(log, "connect 192.168.1.42:5555\n");
    CHECK_STR_HAS(log, "-s 192.168.1.42:5555 push ");
    free(log);
}

TEST(a_cable_beats_the_network_when_asked_to)
{
    char *log;
    kr_server_opts o = {.connect = "192.168.1.42", .connect_fallback = true};
    result r = over_the_network("", o, &log);
    CHECK_MSG(r.ready, "not ready: %s", r.error);
    CHECK_STR_EQ(r.serial, "FAKE123");
    CHECK(log && !strstr(log, "connect"));
    free(log);

    o.serial = NULL;
    r = over_the_network("nousb", o, &log);
    CHECK_MSG(r.ready, "not ready: %s", r.error);
    CHECK_STR_EQ(r.serial, "192.168.1.42:5555");
    CHECK_STR_HAS(log, "connect 192.168.1.42:5555\n");
    free(log);
}

TEST(a_phone_on_usb_is_switched_to_wifi)
{
    char *log;
    kr_server_opts o = {.tcpip = true};
    result r = over_the_network("usb+tcpip", o, &log);
    CHECK_MSG(r.ready, "not ready: %s", r.error);
    CHECK_STR_EQ(r.serial, "192.168.1.42:5555"); /* not the cable's */
    CHECK(log);
    CHECK_STR_HAS(log, "-s FAKE123 shell ip route\n");
    CHECK_STR_HAS(log, "-s FAKE123 tcpip 5555\n");
    /* adbd was not listening at once: it was tried again. */
    char *first = strstr(log, "connect 192.168.1.42:5555\n");
    CHECK(first && strstr(first + 1, "connect 192.168.1.42:5555\n"));
    CHECK_STR_HAS(log, "-s 192.168.1.42:5555 push ");
    free(log);
}

TEST(wifi_failures_are_explained)
{
    char *log;
    kr_server_opts o = {.tcpip = true};
    result r = over_the_network("nowifi", o, &log);
    CHECK(!r.ready && r.failed);
    CHECK_STR_HAS(r.error, "not on Wi-Fi");
    free(log);

    o = (kr_server_opts){.connect = "192.168.1.42"};
    r = over_the_network("refused", o, &log);
    CHECK(!r.ready && r.failed);
    CHECK_STR_HAS(r.error, "Could not connect to 192.168.1.42:5555");
    CHECK_STR_HAS(r.error, "Connection refused");
    free(log);

    o = (kr_server_opts){.connect = ""};
    o.connect = "not an address\n";
    r = over_the_network("refused", o, &log); /* adb is asked and says no */
    CHECK(!r.ready && r.failed);
    free(log);
}

TEST(an_unauthorized_phone_is_explained)
{
    expect_failure("unauthorized", 0, "USB debugging prompt", NULL);
}

TEST(a_failed_push_says_what_adb_said)
{
    expect_failure("push-fails", 0, "Could not copy the server", "Read-only file system");
}

TEST(a_server_that_dies_says_its_last_words)
{
    expect_failure("dies", 0, "stopped", "does not match");
}

TEST(a_server_that_never_connects_times_out)
{
    expect_failure("silent", 1, "did not connect", NULL);
}

TEST(no_adb_at_all)
{
    setup("");
    setenv("KIREMOTE_ADB", "/nonexistent/adb", 1);
    kr_procs *ps = kr_procs_new();
    result r = {.fd = -1};
    kr_server_opts o = {.jar = "/dev/null"};
    static const kr_server_cb cb = {on_ready, on_error, on_log};
    kr_server *s = kr_server_start(ps, &o, &cb, &r);
    run(ps, s, &r);
    kr_server_free(s);
    kr_procs_free(ps);
    teardown();
    CHECK(r.failed);
    CHECK_STR_HAS(r.error, "/nonexistent/adb");
}

int main(void)
{
    RUN(the_server_starts_and_its_socket_is_handed_over);
    RUN(audio_is_a_socket_between_video_and_control);
    RUN(a_phone_is_connected_by_address);
    RUN(a_cable_beats_the_network_when_asked_to);
    RUN(a_phone_on_usb_is_switched_to_wifi);
    RUN(wifi_failures_are_explained);
    RUN(an_unauthorized_phone_is_explained);
    RUN(a_failed_push_says_what_adb_said);
    RUN(a_server_that_dies_says_its_last_words);
    RUN(a_server_that_never_connects_times_out);
    RUN(no_adb_at_all);
    return harness_report();
}
