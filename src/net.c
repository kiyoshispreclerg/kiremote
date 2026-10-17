/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE /* getifaddrs, IFF_* */
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool set_flags(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static int fail_close(int fd)
{
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}

int kr_udp_open(int port, bool bind_it)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int on = 1;
    if (!set_flags(fd) || setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) < 0)
        return fail_close(fd);
    if (bind_it) {
        struct sockaddr_in sa = {.sin_family = AF_INET,
                                 .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_ANY)};
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0 ||
            bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
            return fail_close(fd);
    }
    return fd;
}

int kr_tcp_listen(int first, int last, int *port)
{
    for (int p = first; p <= last; p++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        struct sockaddr_in sa = {.sin_family = AF_INET,
                                 .sin_port = htons((uint16_t)p),
                                 .sin_addr.s_addr = htonl(INADDR_ANY)};
        int on = 1;
        /* SO_REUSEADDR on TCP only skips TIME_WAIT; a live listener still
         * makes bind fail, and we move on to the next port. */
        if (set_flags(fd) && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0 &&
            bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(fd, 16) == 0) {
            *port = p;
            return fd;
        }
        int e = errno;
        close(fd);
        if (e != EADDRINUSE && e != EACCES) {
            errno = e;
            return -1;
        }
    }
    errno = EADDRINUSE;
    return -1;
}

int kr_broadcast_addrs(struct in_addr *out, int max)
{
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0)
        return 0;
    int n = 0;
    for (struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            !(i->ifa_flags & IFF_RUNNING) || (i->ifa_flags & IFF_LOOPBACK) ||
            !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr)
            continue;
        struct in_addr a = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;
        bool dup = false;
        for (int k = 0; k < n; k++)
            dup |= out[k].s_addr == a.s_addr;
        if (!dup && a.s_addr)
            out[n++] = a;
    }
    freeifaddrs(ifs);
    return n;
}

int kr_netlink_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK,
                             .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR};
    if (!set_flags(fd) || bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        return fail_close(fd);
    return fd;
}

bool kr_netlink_changed(int fd)
{
    bool changed = false;
    char buf[8192];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n);
             h = NLMSG_NEXT(h, n))
            if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_NEWLINK)
                changed = true;
    }
    return changed;
}

const char *kr_addr_str(const struct in_addr *a, char *buf, int size)
{
    if (!inet_ntop(AF_INET, a, buf, (socklen_t)size))
        return "?";
    return buf;
}
