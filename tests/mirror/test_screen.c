/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mirror's window and its clipboard, in a private X server
 * (tests/mirror/run.sh): what the window copies is read by another client
 * (xsel), and what another client copies is seen by the window. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../harness.h"
#include "screen.h"
#include "util.h"

typedef struct {
    int changed;
    int got;
    char text[256];
    bool had_text;
} seen;

static void on_changed(void *ud)
{
    ((seen *)ud)->changed++;
}

static void on_text(void *ud, const char *text, size_t len)
{
    seen *s = ud;
    s->got++;
    s->had_text = text != NULL;
    snprintf(s->text, sizeof s->text, "%.*s", (int)(text ? len : 0), text ? text : "");
}

/* Serves the window until fd is readable/closed or ms pass. */
static void pump(kr_screen *scr, int fd, int ms)
{
    double end = kr_now() + ms / 1000.0;
    while (kr_now() < end) {
        struct pollfd p[2] = {{.fd = kr_screen_fd(scr), .events = POLLIN}, {.fd = fd, .events = POLLIN}};
        kr_screen_dispatch(scr);
        if (poll(p, fd >= 0 ? 2 : 1, 20) > 0 && fd >= 0 && (p[1].revents & (POLLIN | POLLHUP)))
            return;
    }
}

/* Runs a command, its stdout into out (while serving the window). */
static void run_xsel(kr_screen *scr, char *const argv[], const char *input, char *out, size_t size)
{
    int o[2], i[2];
    CHECK(pipe(o) == 0 && pipe(i) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(i[0], 0);
        dup2(o[1], 1);
        close(o[0]);
        close(o[1]);
        close(i[0]);
        close(i[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(o[1]);
    close(i[0]);
    if (input && write(i[1], input, strlen(input)) < 0)
        abort();
    close(i[1]);
    size_t n = 0;
    double end = kr_now() + 5;
    while (kr_now() < end && n + 1 < size) {
        pump(scr, o[0], 200);
        struct pollfd p = {.fd = o[0], .events = POLLIN};
        if (poll(&p, 1, 0) > 0) {
            ssize_t r = read(o[0], out + n, size - 1 - n);
            if (r <= 0)
                break;
            n += (size_t)r;
        }
    }
    out[n] = 0;
    close(o[0]);
    if (input) {
        /* xsel -i stays around holding the selection: it ends with the X server. */
        waitpid(pid, NULL, WNOHANG);
    } else {
        waitpid(pid, NULL, 0);
    }
}

TEST(what_the_window_copies_others_can_read)
{
    const char *why = NULL;
    kr_screen *scr = kr_screen_new("test", 200, 400, &why);
    CHECK_MSG(scr, "no window: %s", why);
    if (!scr)
        return;
    CHECK(!kr_screen_clipboard_mine(scr));
    kr_screen_clipboard_set(scr, "olá, mundo ç", strlen("olá, mundo ç"));
    CHECK(kr_screen_clipboard_mine(scr));
    char out[256];
    char *const get[] = {"xsel", "-b", "-o", NULL};
    run_xsel(scr, get, NULL, out, sizeof out);
    CHECK_STR_EQ(out, "olá, mundo ç");

    /* Something newer replaces it. */
    kr_screen_clipboard_set(scr, "outro", 5);
    run_xsel(scr, get, NULL, out, sizeof out);
    CHECK_STR_EQ(out, "outro");
    kr_screen_free(scr);
}

TEST(what_others_copy_the_window_hears_of_and_reads)
{
    const char *why = NULL;
    kr_screen *scr = kr_screen_new("test", 200, 400, &why);
    CHECK(scr);
    if (!scr)
        return;
    seen s = {0};
    kr_screen_clipboard_watch(scr, on_changed, &s);
    kr_screen_clipboard_set(scr, "ours", 4);
    CHECK(kr_screen_clipboard_mine(scr));

    char out[256];
    char *const put[] = {"xsel", "-b", "-i", NULL};
    run_xsel(scr, put, "copiado fora, ã", out, sizeof out);
    pump(scr, -1, 500);
    CHECK_MSG(s.changed >= 1, "the window was not told");
    CHECK(!kr_screen_clipboard_mine(scr)); /* it lost the clipboard */

    kr_screen_clipboard_get(scr, on_text, &s);
    pump(scr, -1, 1000);
    CHECK_INT_EQ(s.got, 1);
    CHECK(s.had_text);
    CHECK_STR_EQ(s.text, "copiado fora, ã");
    kr_screen_free(scr);
}

TEST(asking_when_nobody_has_it)
{
    const char *why = NULL;
    kr_screen *scr = kr_screen_new("test", 200, 400, &why);
    CHECK(scr);
    if (!scr)
        return;
    seen s = {0};
    char out[64];
    char *const clear[] = {"xsel", "-b", "-c", NULL};
    run_xsel(scr, clear, NULL, out, sizeof out);
    kr_screen_clipboard_get(scr, on_text, &s);
    pump(scr, -1, 1000);
    CHECK_INT_EQ(s.got, 1);
    CHECK(!s.had_text);
    kr_screen_free(scr);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN(what_the_window_copies_others_can_read);
    RUN(what_others_copy_the_window_hears_of_and_reads);
    RUN(asking_when_nobody_has_it);
    return harness_report();
}
