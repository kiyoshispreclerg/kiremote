/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The device list from `adb devices -l` (adb.h). */
#include "adb.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

const char *kr_adb_program(void)
{
    const char *p = getenv("KIREMOTE_ADB");
    return p && *p ? p : "adb";
}

/* The states adb prints (scrcpy's is_device_state, from adb.cpp). */
static bool is_state(const char *s, size_t n)
{
    static const char *const states[] = {"device",   "unauthorized", "offline",  "bootloader", "host",
                                         "recovery", "rescue",       "sideload", "authorizing", "connecting",
                                         "detached"};
    for (size_t i = 0; i < sizeof states / sizeof *states; i++)
        if (strlen(states[i]) == n && !memcmp(states[i], s, n))
            return true;
    return false;
}

static void copy(char *dst, size_t size, const char *s, size_t n)
{
    if (n >= size)
        n = size - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* One line, without its "\n". */
static bool parse_line(const char *line, size_t len, kr_adb_dev *d)
{
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    if (!len || line[0] == '*' || !strncmp(line, "adb server", 10))
        return false;
    memset(d, 0, sizeof *d);
    /* From the end, token by token, until the state. */
    size_t end = len;
    for (;;) {
        size_t start = end;
        while (start && line[start - 1] != ' ' && line[start - 1] != '\t')
            start--;
        if (start == end)
            return false;
        const char *tok = line + start;
        size_t tlen = end - start;
        if (tlen > 6 && !memcmp(tok, "model:", 6))
            copy(d->model, sizeof d->model, tok + 6, tlen - 6);
        else if (is_state(tok, tlen)) {
            copy(d->state, sizeof d->state, tok, tlen);
            end = start;
            break;
        }
        end = start;
        while (end && (line[end - 1] == ' ' || line[end - 1] == '\t'))
            end--;
        if (!end)
            return false;
    }
    while (end && (line[end - 1] == ' ' || line[end - 1] == '\t'))
        end--;
    if (!end || end >= sizeof d->serial)
        return false;
    copy(d->serial, sizeof d->serial, line, end);
    return true;
}

int kr_adb_parse_devices(const char *out, kr_adb_dev *devs, int max)
{
    static const char header[] = "List of devices attached";
    const char *p = out;
    bool found = false;
    int n = 0;
    while (*p && n < max) {
        size_t len = strcspn(p, "\n");
        if (!found)
            found = !strncmp(p, header, sizeof header - 1);
        else if (parse_line(p, len, &devs[n]))
            n++;
        p += len;
        if (*p)
            p++;
    }
    return n;
}

int kr_adb_pick(const kr_adb_dev *devs, int n, const char *serial, const char **why)
{
    int idx = -1, ready = 0;
    for (int i = 0; i < n; i++) {
        if (serial && *serial) {
            if (!strcmp(devs[i].serial, serial))
                idx = i;
        } else if (!strcmp(devs[i].state, "device")) {
            idx = i;
            ready++;
        }
    }
    if (serial && *serial) {
        if (idx < 0) {
            *why = _("That device is not connected to adb");
            return -1;
        }
    } else if (ready > 1) {
        *why = _("More than one device is connected: choose one with --serial");
        return -1;
    } else if (idx < 0) {
        /* Say why the ones that are there cannot be used. */
        for (int i = 0; i < n; i++)
            if (!strcmp(devs[i].state, "unauthorized")) {
                *why = _("The phone has not allowed this computer yet: accept the USB debugging prompt on it");
                return -1;
            }
        *why = n ? _("The phone is not ready for adb (offline)")
                 : _("No phone found by adb: connect it by USB with USB debugging on");
        return -1;
    }
    if (!strcmp(devs[idx].state, "unauthorized")) {
        *why = _("The phone has not allowed this computer yet: accept the USB debugging prompt on it");
        return -1;
    }
    if (strcmp(devs[idx].state, "device")) {
        *why = _("The phone is not ready for adb (offline)");
        return -1;
    }
    return idx;
}

bool kr_adb_host_port(const char *host, char *out, size_t size)
{
    if (!host || !*host)
        return false;
    int n = strchr(host, ':') ? snprintf(out, size, "%s", host) : snprintf(out, size, "%s:%d", host, KR_ADB_TCPIP_PORT);
    return n > 0 && (size_t)n < size;
}

bool kr_adb_is_tcpip(const char *serial)
{
    /* "adb-XXXX-YYYY._adb-tls-connect._tcp" (mDNS) has no port to speak of. */
    return strchr(serial, ':') && strncmp(serial, "adb-", 4) != 0;
}

/* The word after `key` in a whitespace-separated line, into out. */
static bool word_after(const char *line, size_t len, const char *key, char *out, size_t size)
{
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < len) {
        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            i++;
        size_t start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t')
            i++;
        if (i - start == klen && !memcmp(line + start, key, klen)) {
            while (i < len && (line[i] == ' ' || line[i] == '\t'))
                i++;
            size_t v = i;
            while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
                i++;
            if (i == v || i - v >= size)
                return false;
            memcpy(out, line + v, i - v);
            out[i - v] = 0;
            return true;
        }
    }
    return false;
}

bool kr_adb_parse_device_ip(const char *out, char *ip, size_t size)
{
    for (const char *p = out; *p;) {
        size_t len = strcspn(p, "\n");
        char dev[32], src[INET_ADDRSTRLEN];
        struct in_addr a;
        if (word_after(p, len, "dev", dev, sizeof dev) && !strncmp(dev, "wlan", 4) &&
            word_after(p, len, "src", src, sizeof src) && inet_pton(AF_INET, src, &a) == 1) {
            if (strlen(src) >= size)
                return false;
            memcpy(ip, src, strlen(src) + 1);
            return true;
        }
        p += len;
        if (*p)
            p++;
    }
    return false;
}

bool kr_adb_connect_ok(const char *out)
{
    return !strncmp(out, "connected", 9) || !strncmp(out, "already connected", 17);
}
