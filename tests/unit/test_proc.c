/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Child processes: output captured whole and by lines, exit status, kill,
 * detach, and a PATH search. */
#include <poll.h>
#include <stdio.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "proc.h"
#include "util.h"

static struct {
    int exits;
    int status;
    char out[256];
    char lines[8][64];
    int n_lines;
} got;

static void on_line(void *ud, const char *line)
{
    (void)ud;
    if (got.n_lines < 8)
        snprintf(got.lines[got.n_lines++], 64, "%s", line);
}

static void on_exit_cb(void *ud, int status, const char *out, size_t len)
{
    (void)ud;
    got.exits++;
    got.status = status;
    snprintf(got.out, sizeof got.out, "%.*s", (int)len, out ? out : "(none)");
}

/* Runs the loop until pred is false or two seconds pass. */
static void run(kr_procs *ps, int *waiting_exits)
{
    double start = kr_now();
    while (got.exits < *waiting_exits && kr_now() - start < 2) {
        struct pollfd fds[8];
        int n = kr_procs_pollfds(ps, fds, 8);
        poll(fds, (nfds_t)n, 50);
        kr_procs_dispatch(ps, fds, n);
    }
}

TEST(captures_output_and_status)
{
    memset(&got, 0, sizeof got);
    kr_procs *ps = kr_procs_new();
    const char *argv[] = {"sh", "-c", "printf 'one\\ntwo\\n'; echo \"$LC_ALL\"; exit 3", NULL};
    kr_proc *p = kr_proc_spawn(ps, argv, true, &(kr_proc_cb){.on_line = on_line, .on_exit = on_exit_cb}, NULL);
    CHECK(p);
    int want = 1;
    run(ps, &want);
    CHECK_INT_EQ(got.exits, 1);
    CHECK(WIFEXITED(got.status) && WEXITSTATUS(got.status) == 3);
    CHECK_STR_EQ(got.out, "one\ntwo\nC\n");     /* LC_ALL=C, so the output can be parsed */
    CHECK_INT_EQ(got.n_lines, 3);
    CHECK_STR_EQ(got.lines[1], "two");
    kr_procs_free(ps);
}

TEST(no_capture_without_a_callback)
{
    memset(&got, 0, sizeof got);
    kr_procs *ps = kr_procs_new();
    const char *argv[] = {"true", NULL};
    CHECK(kr_proc_spawn(ps, argv, false, NULL, NULL)); /* fire and forget */
    const char *bad[] = {"/nonexistent/program", NULL};
    CHECK(!kr_proc_spawn(ps, bad, false, NULL, NULL));
    /* It is reaped, not left a zombie. */
    double start = kr_now();
    while (kr_now() - start < 1) {
        struct pollfd fds[8];
        int n = kr_procs_pollfds(ps, fds, 8);
        poll(fds, (nfds_t)n, 50);
        kr_procs_dispatch(ps, fds, n);
        if (n == 0)
            break;
    }
    CHECK_INT_EQ(kr_procs_pollfds(ps, (struct pollfd[8]){0}, 8), 0);
    kr_procs_free(ps);
}

TEST(kill_and_detach)
{
    memset(&got, 0, sizeof got);
    kr_procs *ps = kr_procs_new();
    const char *argv[] = {"sleep", "30", NULL};
    kr_proc *p = kr_proc_spawn(ps, argv, true, &(kr_proc_cb){.on_exit = on_exit_cb}, NULL);
    CHECK(p);
    kr_proc_kill(p);
    int want = 1;
    run(ps, &want);
    CHECK_INT_EQ(got.exits, 1);
    CHECK(WIFSIGNALED(got.status));

    /* A detached process ends without calling back. */
    memset(&got, 0, sizeof got);
    p = kr_proc_spawn(ps, argv, true, &(kr_proc_cb){.on_exit = on_exit_cb}, NULL);
    CHECK(p);
    kr_proc_detach(p);
    kr_proc_kill(p);
    double start = kr_now();
    while (kr_now() - start < 2) {
        struct pollfd fds[8];
        int n = kr_procs_pollfds(ps, fds, 8);
        if (n == 0)
            break;
        poll(fds, (nfds_t)n, 50);
        kr_procs_dispatch(ps, fds, n);
    }
    CHECK_INT_EQ(got.exits, 0);
    kr_procs_free(ps);
}

TEST(long_lines_do_not_overflow)
{
    memset(&got, 0, sizeof got);
    kr_procs *ps = kr_procs_new();
    const char *argv[] = {"sh", "-c", "head -c 20000 /dev/zero | tr '\\0' x; echo; echo after", NULL};
    CHECK(kr_proc_spawn(ps, argv, false, &(kr_proc_cb){.on_line = on_line, .on_exit = on_exit_cb}, NULL));
    int want = 1;
    run(ps, &want);
    CHECK_INT_EQ(got.exits, 1);
    CHECK_INT_EQ(got.n_lines, 1); /* the huge line is dropped, the next one kept */
    CHECK_STR_EQ(got.lines[0], "after");
    kr_procs_free(ps);
}

TEST(finds_programs_in_path)
{
    CHECK(kr_which("sh"));
    CHECK(kr_which("/bin/sh"));
    CHECK(!kr_which("definitely-not-a-program"));
}

int main(void)
{
    RUN(captures_output_and_status);
    RUN(no_capture_without_a_callback);
    RUN(kill_and_detach);
    RUN(long_lines_do_not_overflow);
    RUN(finds_programs_in_path);
    return harness_report();
}
