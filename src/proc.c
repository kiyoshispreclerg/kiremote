/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* pipe2, syscall */
#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

extern char **environ;

#define MAX_CAPTURE (4u << 20)

struct kr_proc {
    kr_procs *set;
    pid_t pid;
    int pidfd; /* -1: not available, then the exit is looked for on every turn */
    int out;   /* read end of stdout, -1 once at EOF or when not wanted */
    bool capture, exited, detached;
    int status;
    kr_proc_cb cb;
    void *ud;
    char *buf; /* capture: everything so far */
    size_t buf_len, buf_cap;
    char line[8192]; /* line mode: the line being assembled */
    size_t line_len;
    bool line_cut; /* the line outgrew `line`: dropped up to its end */
};

struct kr_procs {
    kr_proc **v;
    int n, cap;
    /* The pollfds handed out last, to match revents back to processes. */
    struct {
        kr_proc *p;
    } slots[64];
};

kr_procs *kr_procs_new(void)
{
    return calloc(1, sizeof(kr_procs));
}

static void proc_free(kr_proc *p)
{
    if (p->out >= 0)
        close(p->out);
    if (p->pidfd >= 0)
        close(p->pidfd);
    free(p->buf);
    free(p);
}

void kr_procs_free(kr_procs *ps)
{
    if (!ps)
        return;
    for (int i = 0; i < ps->n; i++) {
        kr_proc *p = ps->v[i];
        if (!p->exited) {
            kill(p->pid, SIGKILL); /* shutting down: no time to ask nicely */
            waitpid(p->pid, NULL, 0);
        }
        proc_free(p);
    }
    free(ps->v);
    free(ps);
}

/* environ with LC_ALL=C and no LANGUAGE, as a NULL-terminated array. */
static char **make_env(void)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    char **env = calloc(n + 2, sizeof *env);
    if (!env)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "LC_ALL=", 7) != 0 && strncmp(environ[i], "LANGUAGE=", 9) != 0)
            env[k++] = environ[i];
    env[k++] = (char *)"LC_ALL=C";
    return env;
}

bool kr_which(const char *argv0)
{
    if (strchr(argv0, '/'))
        return access(argv0, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        size_t n = strcspn(path, ":");
        char full[4096];
        size_t len = strlen(argv0);
        if (n && n + len + 2 < sizeof full) {
            memcpy(full, path, n);
            full[n] = '/';
            memcpy(full + n + 1, argv0, len + 1);
            if (access(full, X_OK) == 0)
                return true;
        }
        path += n + (path[n] == ':');
    }
    return false;
}

kr_proc *kr_proc_spawn(kr_procs *ps, const char *const *argv, bool capture, const kr_proc_cb *cb,
                       void *ud)
{
    return kr_proc_spawn_ex(ps, argv, capture ? KR_PROC_CAPTURE : 0, cb, ud);
}

kr_proc *kr_proc_spawn_ex(kr_procs *ps, const char *const *argv, unsigned flags, const kr_proc_cb *cb,
                          void *ud)
{
    bool capture = flags & KR_PROC_CAPTURE;
    bool want_out = capture || (cb && cb->on_line);
    int pipefd[2] = {-1, -1};
    if (want_out && pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0)
        return NULL;

    kr_proc *p = calloc(1, sizeof *p);
    char **env = (flags & KR_PROC_USER) ? environ : make_env();
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    bool fa_ok = posix_spawn_file_actions_init(&fa) == 0;
    bool attr_ok = posix_spawnattr_init(&attr) == 0;
    if (attr_ok) {
        /* The daemon blocks SIGTERM/SIGINT (signalfd) and ignores SIGPIPE;
         * children would inherit that and ignore kr_proc_kill()'s SIGTERM,
         * so give them a clean mask and default dispositions. */
        sigset_t none, all;
        sigemptyset(&none);
        sigfillset(&all);
        posix_spawnattr_setsigmask(&attr, &none);
        posix_spawnattr_setsigdefault(&attr, &all);
        short af = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
        if (flags & KR_PROC_USER)
            af |= POSIX_SPAWN_SETSID;
        posix_spawnattr_setflags(&attr, af);
    }
    int err = ENOMEM;
    if (p && env && fa_ok) {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        if (want_out)
            posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
        else
            posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        if (want_out && (flags & KR_PROC_STDERR))
            posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
        else
            posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        pid_t pid;
        err = posix_spawnp(&pid, argv[0], &fa, attr_ok ? &attr : NULL, (char *const *)argv, env);
        if (err == 0)
            p->pid = pid;
    }
    if (fa_ok)
        posix_spawn_file_actions_destroy(&fa);
    if (attr_ok)
        posix_spawnattr_destroy(&attr);
    if (!(flags & KR_PROC_USER))
        free(env);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    if (err != 0) {
        if (pipefd[0] >= 0)
            close(pipefd[0]);
        free(p);
        errno = err ? err : ENOMEM;
        return NULL;
    }
    if (ps->n == ps->cap) {
        int cap = ps->cap ? ps->cap * 2 : 8;
        kr_proc **v = realloc(ps->v, (size_t)cap * sizeof *v);
        if (!v) {
            kill(p->pid, SIGTERM);
            waitpid(p->pid, NULL, 0);
            if (pipefd[0] >= 0)
                close(pipefd[0]);
            free(p);
            errno = ENOMEM;
            return NULL;
        }
        ps->v = v;
        ps->cap = cap;
    }
    p->set = ps;
    p->out = pipefd[0];
    p->capture = capture;
    p->pidfd = (int)syscall(SYS_pidfd_open, p->pid, 0);
    if (p->pidfd >= 0)
        fcntl(p->pidfd, F_SETFD, FD_CLOEXEC);
    if (cb)
        p->cb = *cb;
    p->ud = ud;
    ps->v[ps->n++] = p;
    return p;
}

void kr_proc_kill(kr_proc *p)
{
    if (!p->exited)
        kill(p->pid, SIGTERM);
}

void kr_proc_detach(kr_proc *p)
{
    p->detached = true;
}

/* ---- output ---------------------------------------------------------------- */

static void feed_lines(kr_proc *p, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            if (!p->line_cut && p->cb.on_line && !p->detached) {
                p->line[p->line_len] = '\0';
                p->cb.on_line(p->ud, p->line);
            }
            p->line_len = 0;
            p->line_cut = false;
        } else if (p->line_len + 1 < sizeof p->line) {
            p->line[p->line_len++] = data[i];
        } else {
            p->line_cut = true;
        }
    }
}

/* Reads what the pipe has. Closes it at EOF. */
static void drain(kr_proc *p)
{
    char tmp[8192];
    while (p->out >= 0) {
        ssize_t n = read(p->out, tmp, sizeof tmp);
        if (n > 0) {
            if (p->capture && p->buf_len < MAX_CAPTURE) {
                size_t take = (size_t)n;
                if (take > MAX_CAPTURE - p->buf_len)
                    take = MAX_CAPTURE - p->buf_len;
                if (p->buf_len + take + 1 > p->buf_cap) {
                    size_t cap = p->buf_cap ? p->buf_cap * 2 : 4096;
                    while (cap < p->buf_len + take + 1)
                        cap *= 2;
                    char *b = realloc(p->buf, cap);
                    if (!b)
                        take = 0;
                    else {
                        p->buf = b;
                        p->buf_cap = cap;
                    }
                }
                if (take) {
                    memcpy(p->buf + p->buf_len, tmp, take);
                    p->buf_len += take;
                }
            }
            if (p->cb.on_line)
                feed_lines(p, tmp, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        close(p->out); /* EOF or error */
        p->out = -1;
    }
}

/* Has it exited? Reaps it if so. */
static void check_exit(kr_proc *p)
{
    if (p->exited)
        return;
    int st;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid) {
        p->exited = true;
        p->status = st;
    } else if (r < 0 && errno != EINTR) {
        p->exited = true;
        p->status = -1;
    }
}

/* ---- the loop's side --------------------------------------------------------- */

int kr_procs_pollfds(kr_procs *ps, struct pollfd *fds, int max)
{
    int n = 0;
    for (int i = 0; i < ps->n; i++) {
        kr_proc *p = ps->v[i];
        if (p->out >= 0 && n < max && n < 64) {
            ps->slots[n].p = p;
            fds[n++] = (struct pollfd){.fd = p->out, .events = POLLIN};
        }
        if (!p->exited && p->pidfd >= 0 && n < max && n < 64) {
            ps->slots[n].p = p;
            fds[n++] = (struct pollfd){.fd = p->pidfd, .events = POLLIN};
        }
    }
    return n;
}

void kr_procs_dispatch(kr_procs *ps, const struct pollfd *fds, int n)
{
    (void)fds;
    (void)n;
    /* Few and short-lived: look at all of them. Without a pidfd a process
     * is only noticed when its pipe closes or something else wakes the
     * loop; the daemon asks for a short timeout while any is running (see
     * the daemon's source deadline). */
    for (int i = 0; i < ps->n;) {
        kr_proc *p = ps->v[i];
        drain(p);
        check_exit(p);
        if (p->exited) {
            drain(p); /* what it wrote just before ending */
            if (p->out >= 0) { /* a grandchild may still hold the pipe */
                close(p->out);
                p->out = -1;
            }
        }
        if (!p->exited) {
            i++;
            continue;
        }
        ps->v[i] = ps->v[--ps->n];
        if (!p->detached && p->cb.on_exit) {
            if (p->capture && !p->buf)
                p->buf = calloc(1, 1);
            if (p->buf)
                p->buf[p->buf_len] = '\0';
            p->cb.on_exit(p->ud, p->status, p->capture ? p->buf : NULL, p->buf_len);
        }
        proc_free(p);
    }
}

double kr_procs_deadline(const kr_procs *ps)
{
    /* Only for children we can't get a pidfd for. */
    for (int i = 0; i < ps->n; i++)
        if (ps->v[i]->pidfd < 0 && !ps->v[i]->exited)
            return kr_now() + 0.2;
    return 0;
}
