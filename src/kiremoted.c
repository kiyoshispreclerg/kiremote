/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiremoted: the daemon. One thread, one poll() loop; everything (the
 * KDE Connect sockets, X, D-Bus, the control socket) hangs off it as it
 * arrives in later phases. For now it reads the configuration and waits
 * for SIGINT/SIGTERM. */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "config.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

static void usage(FILE *out)
{
    fprintf(out, _("Usage: %s [OPTION]...\n"
                   "Connects this computer to your phone (KDE Connect protocol).\n"
                   "\n"
                   "  -c, --config FILE  configuration file\n"
                   "                     (default ~/.config/kiremote/kiremote.ini)\n"
                   "  -v, --verbose      debug messages\n"
                   "  -V, --version      show the version and exit\n"
                   "  -h, --help         show this help and exit\n"),
            "kiremoted");
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

int main(int argc, char **argv)
{
    kr_i18n_init();
    kr_log_init("kiremoted");

    static const struct option longopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *config_file = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "c:vVh", longopts, NULL)) != -1) {
        switch (c) {
        case 'c':
            config_file = optarg;
            break;
        case 'v':
            kr_log_set_verbose(true);
            break;
        case 'V':
            printf("kiremoted %s\n", KR_VERSION);
            return 0;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind < argc) {
        usage(stderr);
        return 2;
    }

    kr_config *cfg = kr_config_load(config_file);
    if (!cfg)
        return 1;
    kr_log_info(_("device name: %s"), kr_config_device_name(cfg));

    int sfd = open_signal_fd();
    if (sfd < 0) {
        kr_log_error(_("cannot watch signals: %s"), strerror(errno));
        kr_config_free(cfg);
        return 1;
    }

    bool running = true;
    while (running) {
        struct pollfd fds[1] = {{.fd = sfd, .events = POLLIN}};
        if (poll(fds, 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            kr_log_error("poll: %s", strerror(errno));
            break;
        }
        if (fds[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                if (si.ssi_signo == SIGHUP) {
                    kr_log_debug("SIGHUP: reload not implemented yet");
                    continue;
                }
                running = false;
            }
        }
    }

    close(sfd);
    kr_config_free(cfg);
    return 0;
}
