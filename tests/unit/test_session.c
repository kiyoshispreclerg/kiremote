/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One kiremote-mirror per phone: claiming, asking, raising, quitting, and a
 * socket left behind by a process that died. The other side runs in a child
 * process because the owner answers from this one's loop. */
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../harness.h"
#include "session.h"
#include "util.h"

static int raised, quit;

static void on_raise(void *ud)
{
    (void)ud;
    raised++;
}

static void on_quit(void *ud)
{
    (void)ud;
    quit++;
}

static const kr_inst_cbs cbs = {on_raise, on_quit};

/* Runs fn in a child while serving i here; the child's exit status. */
static int meanwhile(kr_inst *i, int (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0)
        _exit(fn());
    int status = -1;
    double end = kr_now() + 5;
    while (kr_now() < end) {
        struct pollfd fds[8];
        int n = i ? kr_inst_pollfds(i, fds, 8) : 0;
        poll(fds, (nfds_t)n, 20);
        if (i)
            kr_inst_dispatch(i, fds, n);
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    kill(pid, 9);
    waitpid(pid, NULL, 0);
    return -1;
}

static int child_query(void)
{
    char mode[16];
    return kr_inst_query("5a0d7fcd", mode, sizeof mode) && !strcmp(mode, "mirror") ? 0 : 1;
}

static int child_raise_then_quit(void)
{
    return kr_inst_send("5a0d7fcd", "raise") && kr_inst_send("5a0d7fcd", "quit") ? 0 : 1;
}

static int child_claim_fails(void)
{
    char other[16];
    kr_inst *i = kr_inst_claim("5a0d7fcd", "camera", &cbs, NULL, other, sizeof other);
    return !i && !strcmp(other, "mirror") ? 0 : 1;
}

static int child_dies_holding(void)
{
    char other[16];
    kr_inst *i = kr_inst_claim("stale", "camera", &cbs, NULL, other, sizeof other);
    return i ? 0 : 1; /* and exits without kr_inst_free: the socket stays */
}

static int child_bad_line(void)
{
    char reply[16];
    return !kr_inst_send("5a0d7fcd", "dance") && !kr_inst_query("nobody-home", reply, sizeof reply) ? 0 : 1;
}

TEST(a_phone_is_taken_once_and_can_be_asked)
{
    char tmp[] = "/tmp/kiremote-inst-XXXXXX";
    CHECK(mkdtemp(tmp));
    setenv("XDG_RUNTIME_DIR", tmp, 1);
    char other[16];
    kr_inst *i = kr_inst_claim("5a0d7fcd", "mirror", &cbs, NULL, other, sizeof other);
    CHECK(i);
    CHECK_STR_EQ(other, "");

    CHECK_INT_EQ(meanwhile(i, child_query), 0);
    CHECK_INT_EQ(meanwhile(i, child_claim_fails), 0); /* the other mode is told */
    CHECK_INT_EQ(meanwhile(i, child_bad_line), 0);
    CHECK_INT_EQ(raised, 0);
    CHECK_INT_EQ(meanwhile(i, child_raise_then_quit), 0);
    CHECK_INT_EQ(raised, 1);
    CHECK_INT_EQ(quit, 1);

    /* The socket is the owner's: gone with it. */
    char path[300];
    snprintf(path, sizeof path, "%s/kiremote-mirror/5a0d7fcd.sock", tmp);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    kr_inst_free(i);
    CHECK(stat(path, &st) != 0);
    char mode[16];
    CHECK(!kr_inst_query("5a0d7fcd", mode, sizeof mode));

    snprintf(path, sizeof path, "rm -rf %s", tmp);
    CHECK(system(path) == 0);
}

TEST(a_socket_left_by_a_dead_process_is_taken_over)
{
    char tmp[] = "/tmp/kiremote-inst-XXXXXX";
    CHECK(mkdtemp(tmp));
    setenv("XDG_RUNTIME_DIR", tmp, 1);
    CHECK_INT_EQ(meanwhile(NULL, child_dies_holding), 0);
    char path[300], other[16];
    snprintf(path, sizeof path, "%s/kiremote-mirror/stale.sock", tmp);
    CHECK(access(path, F_OK) == 0); /* it is there, but nobody answers */
    kr_inst *i = kr_inst_claim("stale", "mirror", &cbs, NULL, other, sizeof other);
    CHECK(i);
    CHECK_STR_EQ(other, "");
    kr_inst_free(i);

    /* A key that is not a file name is made into one. */
    i = kr_inst_claim("192.168.0.7:5555 / x", "mirror", &cbs, NULL, other, sizeof other);
    CHECK(i);
    kr_inst_free(i);
    snprintf(path, sizeof path, "rm -rf %s", tmp);
    CHECK(system(path) == 0);
}

int main(void)
{
    RUN(a_phone_is_taken_once_and_can_be_asked);
    RUN(a_socket_left_by_a_dead_process_is_taken_over);
    return harness_report();
}
