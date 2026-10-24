/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_ADB_H
#define KR_ADB_H

/* What kiremote-mirror needs from adb, without I/O: the program to run and
 * the device list it prints. The commands themselves run through kr_proc
 * (src/proc.h), in the mirror's loop. Read in upstream/scrcpy:
 * app/src/adb/adb.c and adb_parser.c. */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char serial[128];
    char state[24]; /* "device", "unauthorized", "offline"... */
    char model[64]; /* "" if adb did not say */
} kr_adb_dev;

/* $KIREMOTE_ADB, or "adb" from the PATH. */
const char *kr_adb_program(void);

/* `adb devices -l`: the lines after "List of devices attached" (garbage
 * before it, from a server starting, is skipped). The serial may contain
 * spaces, so each line is read from the end up to the state, as scrcpy
 * does. Returns how many were found (at most max). */
int kr_adb_parse_devices(const char *out, kr_adb_dev *devs, int max);

/* The device to use: the one with that serial, or the only one ready if
 * serial is NULL or "". -1 with *why set (a translated sentence) if there
 * is none, it is not ready, or there are several to choose from. */
int kr_adb_pick(const kr_adb_dev *devs, int n, const char *serial, const char **why);

/* ---- over the network ------------------------------------------------------ */

#define KR_ADB_TCPIP_PORT 5555

/* "192.168.0.7" → "192.168.0.7:5555"; "192.168.0.7:37005" is kept. false if
 * it does not fit or is empty. */
bool kr_adb_host_port(const char *host, char *out, size_t size);

/* A serial that is an address (adb's TCP/IP connections), not a USB one. */
bool kr_adb_is_tcpip(const char *serial);

/* The phone's address on Wi-Fi from `adb shell ip route` (the line of a
 * wlan interface, its "src"), as scrcpy finds it. false if there is none. */
bool kr_adb_parse_device_ip(const char *out, char *ip, size_t size);

/* `adb connect` exits 0 even when it fails: only its words say
 * ("connected to" or "already connected to"). */
bool kr_adb_connect_ok(const char *out);

#endif
