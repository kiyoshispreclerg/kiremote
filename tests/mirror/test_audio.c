/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The sound playback against a private PulseAudio server with a null sink
 * (never the user's): the stream connects, PCM is taken, a late burst is cut
 * to what is still timely, and a server that goes away ends the sound
 * without hurting the caller. Skipped where there is no pulseaudio. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../harness.h"
#include "audio.h"
#include "util.h"

static char dir[96];
static pid_t server = -1;

static bool start_server(void)
{
    snprintf(dir, sizeof dir, "/tmp/kiremote-audio-XXXXXX");
    if (!mkdtemp(dir))
        return false;
    char sock[160], load[400];
    snprintf(sock, sizeof sock, "%s/native", dir);
    snprintf(load, sizeof load, "module-native-protocol-unix socket=%s auth-anonymous=1", sock);
    server = fork();
    if (server == 0) {
        setenv("HOME", dir, 1);
        setenv("XDG_RUNTIME_DIR", dir, 1);
        setenv("XDG_CONFIG_HOME", dir, 1);
        unsetenv("PULSE_SERVER");
        unsetenv("DISPLAY");
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
        int null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        execlp("pulseaudio", "pulseaudio", "-n", "--daemonize=no", "--exit-idle-time=-1", "--use-pid-file=no",
               "-L", "module-null-sink sink_name=test", "-L", load, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) { /* up to 5 s for the socket */
        if (access(sock, F_OK) == 0) {
            char url[200];
            snprintf(url, sizeof url, "unix:%s", sock);
            setenv("PULSE_SERVER", url, 1);
            return true;
        }
        int st;
        if (waitpid(server, &st, WNOHANG) == server) {
            server = -1;
            return false;
        }
        poll(NULL, 0, 50);
    }
    return false;
}

static void stop_server(void)
{
    if (server > 0) {
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
    }
    char p[200];
    snprintf(p, sizeof p, "rm -rf %s", dir);
    if (system(p) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
}

/* Runs the audio loop for ms. */
static void spin(kr_audio *a, int ms)
{
    double end = kr_now() + ms / 1000.0;
    struct pollfd none[1] = {{.fd = -1}};
    while (kr_now() < end)
        kr_audio_poll(a, none, 1, 20);
}

/* Until the sound server has taken everything (the null sink is slow to
 * start), or ms. */
static void spin_until_empty(kr_audio *a, int ms)
{
    double end = kr_now() + ms / 1000.0;
    while (kr_audio_queued(a) > 0 && kr_now() < end)
        spin(a, 20);
}

TEST(plays_and_cuts_a_late_burst)
{
    const char *why = NULL;
    kr_audio *a = kr_audio_new("Kiremote test", "Fake phone", &why);
    CHECK_MSG(a, "no audio: %s", why);
    if (!a)
        return;
    spin(a, 500);
    CHECK(kr_audio_ok(a));

    /* The phone's sound as it comes: 10 ms every 10 ms, for a second. */
    static uint8_t chunk[KR_AUDIO_RATE / 100 * KR_AUDIO_FRAME];
    size_t most = 0;
    for (int i = 0; i < 100; i++) {
        kr_audio_play(a, chunk, sizeof chunk);
        if (kr_audio_queued(a) > most)
            most = kr_audio_queued(a);
        spin(a, 10);
    }
    CHECK(kr_audio_ok(a));
    CHECK_MSG(most <= KR_AUDIO_MAX_QUEUE, "%zu queued", most);
    spin_until_empty(a, 5000);
    CHECK_INT_EQ(kr_audio_queued(a), 0);

    /* A late burst, a second of it in one go: only the timely end is kept. */
    static uint8_t second[KR_AUDIO_RATE * KR_AUDIO_FRAME];
    kr_audio_play(a, second, sizeof second);
    CHECK(kr_audio_queued(a) <= KR_AUDIO_MAX_QUEUE);
    spin_until_empty(a, 5000);
    CHECK_INT_EQ(kr_audio_queued(a), 0);

    /* Half a frame waits for the rest. */
    kr_audio_play(a, chunk, 3);
    spin(a, 100);
    CHECK_INT_EQ(kr_audio_queued(a), 3);
    kr_audio_play(a, chunk, 5);
    spin(a, 200);
    CHECK_INT_EQ(kr_audio_queued(a), 0);

    /* The caller's descriptors are polled too. */
    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "x", 1) == 1);
    struct pollfd fds[1] = {{.fd = p[0], .events = POLLIN}};
    CHECK_INT_EQ(kr_audio_poll(a, fds, 1, 1000), 1);
    CHECK(fds[0].revents & POLLIN);
    close(p[0]);
    close(p[1]);
    kr_audio_free(a);
}

TEST(sound_resumes_after_a_silence)
{
    const char *why = NULL;
    kr_audio *a = kr_audio_new("Kiremote test", "Fake phone", &why);
    CHECK(a);
    if (!a)
        return;
    spin(a, 600); /* the phone plays nothing for a while */
    static uint8_t chunk[KR_AUDIO_RATE / 100 * KR_AUDIO_FRAME];
    for (int i = 0; i < 100; i++) {
        kr_audio_play(a, chunk, sizeof chunk);
        spin(a, 10);
    }
    CHECK(kr_audio_ok(a));
    spin_until_empty(a, 5000);
    CHECK_INT_EQ(kr_audio_queued(a), 0);
    kr_audio_free(a);
}

TEST(burst_after_silence)
{
    const char *why = NULL;
    kr_audio *a = kr_audio_new("Kiremote test", "Fake phone", &why);
    CHECK(a);
    if (!a)
        return;
    spin(a, 600);
    static uint8_t second[KR_AUDIO_RATE * KR_AUDIO_FRAME];
    kr_audio_play(a, second, sizeof second);
    spin_until_empty(a, 5000);
    CHECK_INT_EQ(kr_audio_queued(a), 0);
    kr_audio_free(a);
}

TEST(the_server_going_away_ends_only_the_sound)
{
    const char *why = NULL;
    kr_audio *a = kr_audio_new("Kiremote test", "Fake phone", &why);
    CHECK(a);
    if (!a)
        return;
    spin(a, 300);
    CHECK(kr_audio_ok(a));
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    server = -1;
    spin(a, 500);
    CHECK(!kr_audio_ok(a));
    kr_audio_play(a, (const uint8_t *)"abcd", 4); /* ignored, no crash */
    int p[2];
    CHECK(pipe(p) == 0);
    struct pollfd fds[1] = {{.fd = p[0], .events = POLLIN}};
    CHECK_INT_EQ(kr_audio_poll(a, fds, 1, 20), 0); /* plain poll() now */
    close(p[0]);
    close(p[1]);
    kr_audio_free(a);
}

TEST(no_server_is_an_error_not_a_crash)
{
    setenv("PULSE_SERVER", "unix:/nonexistent/pulse", 1);
    const char *why = NULL;
    kr_audio *a = kr_audio_new("Kiremote test", "Fake phone", &why);
    if (a) { /* connecting is asynchronous: it fails a moment later */
        spin(a, 300);
        CHECK(!kr_audio_ok(a));
        kr_audio_free(a);
    } else {
        CHECK(why && *why);
    }
}

int main(void)
{
    if (!start_server()) {
        stop_server();
        printf("test_audio: no private pulseaudio to test against, skipped\n");
        return 0;
    }
    RUN(plays_and_cuts_a_late_burst);
    RUN(sound_resumes_after_a_silence);
    RUN(burst_after_silence);
    RUN(the_server_going_away_ends_only_the_sound);
    RUN(no_server_is_an_error_not_a_crash);
    stop_server();
    return harness_report();
}
