/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_PROC_H
#define KR_PROC_H

/* Child processes that never block the loop: pactl, a sound player. Each
 * one is started with posix_spawnp (no shell: arguments go as they are),
 * LC_ALL=C so the output is the one we parse, stdin and stderr on
 * /dev/null. The standard output is read through a pipe (whole, or line by
 * line for long-lived ones such as `pactl subscribe`), and the end of the
 * process is noticed through a pidfd, so a running child costs no timer.
 *
 * A kr_procs is a source of the daemon's loop (see kr_source in daemon.h);
 * it owns its processes and kills the ones still running when freed. */

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct kr_procs kr_procs;
typedef struct kr_proc kr_proc;

typedef struct {
    /* One line of output (without the "\n"), as it arrives; optional. */
    void (*on_line)(void *ud, const char *line);
    /* The process ended: status as from waitpid (-1 if it could not be
     * waited for), out the whole output when capture was asked, else NULL.
     * Not called after kr_proc_detach. The kr_proc is gone afterwards. */
    void (*on_exit)(void *ud, int status, const char *out, size_t len);
} kr_proc_cb;

kr_procs *kr_procs_new(void);
void kr_procs_free(kr_procs *ps);

/* Starts argv (argv[0] is searched in PATH). capture keeps the whole
 * output for on_exit (up to 4 MiB; the rest is dropped); line mode and
 * capture can be combined. NULL if it could not start (errno). cb may be
 * NULL for fire and forget. */
kr_proc *kr_proc_spawn(kr_procs *ps, const char *const *argv, bool capture, const kr_proc_cb *cb,
                       void *ud);
/* SIGTERM; on_exit still comes when it is gone. */
void kr_proc_kill(kr_proc *p);
/* The owner is going away: no more callbacks; the process runs on (kill it
 * first if it should not). */
void kr_proc_detach(kr_proc *p);

/* The loop's side. */
int kr_procs_pollfds(kr_procs *ps, struct pollfd *fds, int max);
void kr_procs_dispatch(kr_procs *ps, const struct pollfd *fds, int n);
/* Monotonic time to look again (children without a pidfd), 0 for none. */
double kr_procs_deadline(const kr_procs *ps);

/* Is argv0 an executable in PATH? */
bool kr_which(const char *argv0);

#endif
