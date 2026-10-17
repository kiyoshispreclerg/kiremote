/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.battery, both ways (plugins/battery/batteryplugin.cpp and
 * and/plugins/battery upstream):
 *
 *   {"isCharging": bool, "currentCharge": 0-100, "thresholdEvent": 0|1,
 *    "batteryQuantity": n (ours only)}
 *   {"request": true}  asks the other side to send its state
 *
 * thresholdEvent 1 = battery entered the low state: a notification.
 * The upstream asks Solid for the primary battery; here a battery is a
 * /sys/class/power_supply entry of type Battery that is not a peripheral
 * (scope "Device": a wireless mouse is not this computer). A desktop has
 * none, and then nothing is sent, as upstream. Changes come from kernel
 * uevents, so an idle laptop wakes us only when the battery really changes. */
#include <dirent.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "util.h"

#define BATTERY "kdeconnect.battery"
#define LOW_PERCENT 15 /* batteryplugin.cpp: "low if it's below 15%" */

static const char *const types[] = {BATTERY, NULL};

typedef struct {
    int charge;    /* -1: unknown */
    bool charging;
    bool low;      /* thresholdEvent said so */
} peer_battery;

static struct {
    int fd; /* uevent socket, -1 if none */
    char dir[KR_PATH_MAX];
    bool have;               /* the last reading found a battery */
    kr_battery_info last;    /* what we last sent */
    bool sent;
} self = {.fd = -1};

/* ---- reading the system battery ------------------------------------------- */

static bool read_text(const char *dir, const char *name, const char *file, char *out, size_t size)
{
    char path[KR_PATH_MAX];
    if (!kr_pathf(path, sizeof path, "%s/%s/%s", dir, name, file))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (ok)
        out[strcspn(out, "\n")] = '\0';
    return ok;
}

static long read_long(const char *dir, const char *name, const char *file, long fallback)
{
    char buf[64];
    if (!read_text(dir, name, file, buf, sizeof buf))
        return fallback;
    char *end;
    long v = strtol(buf, &end, 10);
    return end == buf ? fallback : v;
}

bool kr_battery_read(const char *dir, kr_battery_info *out)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    *out = (kr_battery_info){0};
    long total = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char type[32], scope[32], status[32];
        if (!read_text(dir, de->d_name, "type", type, sizeof type) || strcmp(type, "Battery") != 0)
            continue;
        if (read_text(dir, de->d_name, "scope", scope, sizeof scope) && strcmp(scope, "Device") == 0)
            continue;
        long cap = read_long(dir, de->d_name, "capacity", -1);
        if (cap < 0) {
            long now = read_long(dir, de->d_name, "energy_now", read_long(dir, de->d_name, "charge_now", -1));
            long full = read_long(dir, de->d_name, "energy_full", read_long(dir, de->d_name, "charge_full", -1));
            if (now < 0 || full <= 0)
                continue;
            cap = now * 100 / full;
        }
        if (cap > 100)
            cap = 100;
        total += cap;
        out->count++;
        if (read_text(dir, de->d_name, "status", status, sizeof status)) {
            if (strcmp(status, "Charging") == 0)
                out->charging = true;
            else if (strcmp(status, "Discharging") == 0)
                out->discharging = true;
        }
    }
    closedir(d);
    if (!out->count)
        return false;
    out->charge = (int)(total / out->count);
    return true;
}

static bool same(const kr_battery_info *a, const kr_battery_info *b)
{
    return a->charge == b->charge && a->charging == b->charging && a->discharging == b->discharging &&
           a->count == b->count;
}

static void send_self(kr_daemon *d, kr_device *dev, const kr_battery_info *b)
{
    kr_packet *p = kr_packet_new(BATTERY);
    if (!p)
        return;
    cJSON_AddBoolToObject(p->body, "isCharging", b->charging);
    cJSON_AddNumberToObject(p->body, "currentCharge", b->charge);
    cJSON_AddNumberToObject(p->body, "batteryQuantity", b->count);
    cJSON_AddNumberToObject(p->body, "thresholdEvent",
                            b->charge <= LOW_PERCENT && b->discharging ? 1 : 0);
    kr_daemon_send(d, dev, p);
    kr_packet_free(p);
}

/* Reads the battery; sends to every device if it changed (or force). */
static void refresh(kr_daemon *d, bool force)
{
    kr_battery_info now;
    self.have = kr_battery_read(self.dir, &now);
    if (!self.have || (!force && self.sent && same(&now, &self.last)))
        return;
    self.last = now;
    self.sent = true;
    for (int i = 0; i < kr_daemon_device_count(d); i++)
        send_self(d, kr_daemon_device_at(d, i), &now);
}

/* ---- uevents ------------------------------------------------------------------ */

static int src_pollfds(void *ud, struct pollfd *fds, int max)
{
    (void)ud;
    if (self.fd < 0 || max < 1)
        return 0;
    fds[0] = (struct pollfd){.fd = self.fd, .events = POLLIN};
    return 1;
}

/* A uevent is "action@path\0KEY=value\0...": look for the subsystem. */
static bool has_power_subsystem(const char *buf, size_t n)
{
    static const char key[] = "SUBSYSTEM=power_supply";
    size_t k = sizeof key - 1;
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(buf + i, key, k) == 0)
            return true;
    return false;
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    kr_daemon *d = ud;
    if (n < 1 || !(fds[0].revents & POLLIN))
        return;
    bool power = false;
    char buf[4096];
    ssize_t r;
    while ((r = recv(self.fd, buf, sizeof buf - 1, MSG_DONTWAIT)) > 0) {
        if (has_power_subsystem(buf, (size_t)r))
            power = true;
    }
    if (power)
        refresh(d, false);
}

static void init(kr_daemon *d)
{
    const char *dir = kr_config_get(kr_daemon_config(d), "battery", "power_supply_dir");
    kr_pathf(self.dir, sizeof self.dir, "%s", dir && *dir ? dir : "/sys/class/power_supply");
    kr_battery_info probe;
    self.have = kr_battery_read(self.dir, &probe);
    if (!self.have) {
        kr_log_debug("no battery in %s: not reporting ours", self.dir);
        return;
    }
    /* Kernel uevents (group 1). May be refused in a container: then we
     * only report on connect and on request. */
    self.fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (self.fd >= 0) {
        struct sockaddr_nl sa = {.nl_family = AF_NETLINK, .nl_groups = 1};
        if (bind(self.fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            close(self.fd);
            self.fd = -1;
        }
    }
    kr_daemon_add_source(d, &(kr_source){src_pollfds, src_dispatch, NULL}, d);
}

static void shutdown_plugin(kr_daemon *d)
{
    kr_daemon_remove_source(d, d);
    if (self.fd >= 0)
        close(self.fd);
    self.fd = -1;
    self.sent = false;
}

/* ---- the plugin ----------------------------------------------------------------- */

static void on_connect(kr_daemon *d, kr_device *dev)
{
    peer_battery *pb = kr_device_state(dev, &kr_plugin_battery);
    pb->charge = -1;
    kr_battery_info now;
    if (kr_battery_read(self.dir, &now))
        send_self(d, dev, &now);
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    peer_battery *pb = kr_device_state(dev, &kr_plugin_battery);
    if (kr_packet_bool(p, "request", false)) {
        kr_battery_info now;
        if (kr_battery_read(self.dir, &now))
            send_self(d, dev, &now);
        return;
    }
    int64_t charge = -1, event = 0;
    kr_packet_int(p, "currentCharge", &charge);
    kr_packet_int(p, "thresholdEvent", &event);
    pb->charge = (int)charge;
    pb->charging = kr_packet_bool(p, "isCharging", false);
    if (event == 1 && !pb->charging && !pb->low) {
        char body[64];
        snprintf(body, sizeof body, _("Battery at %d%%"), pb->charge);
        char title[160];
        snprintf(title, sizeof title, _("%s: Low Battery"), kr_device_name(dev));
        kr_note *n = kr_notify_show(kr_daemon_notify(d),
                                    &(kr_note_spec){.summary = title, .body = body,
                                                    .icon = "battery-low", .timeout_ms = -1},
                                    NULL, NULL);
        if (n)
            kr_note_detach(n);
    }
    pb->low = event == 1;
}

static void describe(kr_daemon *d, const kr_device *dev, cJSON *out)
{
    (void)d;
    peer_battery *pb = kr_device_state((kr_device *)dev, &kr_plugin_battery);
    if (!pb || pb->charge < 0)
        return;
    cJSON *b = cJSON_AddObjectToObject(out, "battery");
    if (b) {
        cJSON_AddNumberToObject(b, "charge", pb->charge);
        cJSON_AddBoolToObject(b, "charging", pb->charging);
    }
}

const kr_plugin kr_plugin_battery = {
    .name = "battery",
    .incoming = types,
    .outgoing = types,
    .state_size = sizeof(peer_battery),
    .init = init,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_packet = on_packet,
    .describe = describe,
};
