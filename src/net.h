/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_NET_H
#define KR_NET_H

/* Sockets for the LAN backend, all non-blocking and close-on-exec:
 * the discovery UDP port, TCP listeners on the first free port of a
 * range, the broadcast addresses of the interfaces, and a netlink socket
 * that says when addresses change (time to announce ourselves again). */

#include <netinet/in.h>
#include <stdbool.h>

#define KR_UDP_PORT 1716
#define KR_TCP_FIRST 1716
#define KR_TCP_LAST 1764
#define KR_PAYLOAD_FIRST 1739
#define KR_PAYLOAD_LAST 1764

/* UDP on 0.0.0.0:port with SO_REUSEADDR and SO_BROADCAST, like the
 * upstream's ShareAddress. With bind false (the port is taken), an unbound
 * socket that can still send broadcasts. -1 on error (errno). */
int kr_udp_open(int port, bool bind_it);

/* TCP listening on 0.0.0.0 at the first free port in [first, last];
 * *port gets it. -1 (errno) if none is free. */
int kr_tcp_listen(int first, int last, int *port);

/* The broadcast address of every IPv4 interface that is up, not loopback
 * and can broadcast. Returns how many (at most max). */
int kr_broadcast_addrs(struct in_addr *out, int max);

/* Address changes, or -1 if netlink is unavailable (not fatal). */
int kr_netlink_open(void);
/* Reads what is pending; true if an address or link changed. */
bool kr_netlink_changed(int fd);

/* "1.2.3.4" for logs (static buffer of the caller). */
const char *kr_addr_str(const struct in_addr *a, char *buf, int size);

#endif
