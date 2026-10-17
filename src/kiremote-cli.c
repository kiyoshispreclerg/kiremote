/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiremote-cli: talks to a running kiremoted over its control socket
 * (control.h): list devices, pair, ping, send files. No GTK. */
#define _DEFAULT_SOURCE /* realpath */
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "control.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

#define TIMEOUT_MS 5000
/* The daemon gives up on a pairing after 30 s. */
#define PAIR_WAIT_S 35.0

static const char *socket_path;

static void usage(FILE *out)
{
    fprintf(out, _("Usage: %s [OPTION]...\n"
                   "Controls kiremoted, the phone connection daemon.\n"
                   "\n"
                   "  -l, --list            list the devices (paired, and the ones around)\n"
                   "  -d, --device ID       the device for the options below (id or name)\n"
                   "      --pair            ask to pair (or accept its request) and wait\n"
                   "      --unpair          unpair (or refuse its request)\n"
                   "      --ping            send a ping\n"
                   "      --ping-msg TEXT   send a ping with a message\n"
                   "      --share FILE      send a file (may be repeated)\n"
                   "      --refresh         announce this computer on the network again\n"
                   "  -s, --socket PATH     the daemon's control socket\n"
                   "  -V, --version         show the version and exit\n"
                   "  -h, --help            show this help and exit\n"),
            "kiremote-cli");
}

/* Sends req (consumed) and returns the reply, or NULL after printing why. */
static cJSON *request(cJSON *req)
{
    char *line = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!line)
        return NULL;
    char *raw = kr_control_request(socket_path, line, TIMEOUT_MS);
    free(line);
    if (!raw) {
        kr_log_error(_("cannot talk to kiremoted at %s: %s"), socket_path, strerror(errno));
        return NULL;
    }
    cJSON *reply = cJSON_Parse(raw);
    free(raw);
    if (!cJSON_IsObject(reply)) {
        cJSON_Delete(reply);
        kr_log_error(_("invalid reply from kiremoted"));
        return NULL;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(reply, "ok"))) {
        const cJSON *e = cJSON_GetObjectItem(reply, "error");
        kr_log_error("%s", cJSON_IsString(e) ? e->valuestring : "?");
        const cJSON *x;
        cJSON_ArrayForEach(x, cJSON_GetObjectItem(reply, "errors"))
        {
            if (cJSON_IsString(x) && x != cJSON_GetArrayItem(cJSON_GetObjectItem(reply, "errors"), 0))
                kr_log_error("%s", x->valuestring);
        }
        cJSON_Delete(reply);
        return NULL;
    }
    return reply;
}

static cJSON *simple(const char *type, const char *device)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", type);
    if (device)
        cJSON_AddStringToObject(req, "device", device);
    return req;
}

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static bool flag(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(o, key));
}

static void print_device(const cJSON *dev)
{
    const char *state = str(dev, "state");
    bool paired = flag(dev, "paired"), reachable = flag(dev, "reachable");
    const char *what;
    if (strcmp(state, "requested_by_peer") == 0)
        what = _("asks to pair");
    else if (strcmp(state, "requested") == 0)
        what = _("waiting for it to accept the pairing");
    else if (paired)
        what = reachable ? _("paired and reachable") : _("paired, not reachable");
    else
        what = _("reachable, not paired");
    printf("- %s: %s (%s", str(dev, "name"), str(dev, "id"), what);
    if (*str(dev, "key"))
        printf(_(", verification key %s"), str(dev, "key"));
    printf(")\n");
}

static int list(void)
{
    cJSON *reply = request(simple("devices", NULL));
    if (!reply)
        return 1;
    const cJSON *a = cJSON_GetObjectItem(reply, "devices"), *dev;
    int n = cJSON_GetArraySize(a);
    cJSON_ArrayForEach(dev, a) print_device(dev);
    printf(ngettext("%d device found\n", "%d devices found\n", (unsigned long)n), n);
    cJSON_Delete(reply);
    return 0;
}

/* The device's entry in a "devices" reply, by id. */
static cJSON *lookup(const char *id)
{
    cJSON *reply = request(simple("devices", NULL));
    if (!reply)
        return NULL;
    cJSON *dev;
    cJSON_ArrayForEach(dev, cJSON_GetObjectItem(reply, "devices"))
    {
        if (strcmp(str(dev, "id"), id) == 0) {
            cJSON *copy = cJSON_Duplicate(dev, true);
            cJSON_Delete(reply);
            return copy;
        }
    }
    cJSON_Delete(reply);
    return NULL;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static int pair(const char *device)
{
    cJSON *reply = request(simple("pair", device));
    if (!reply)
        return 1;
    const cJSON *dev = cJSON_GetObjectItem(reply, "device");
    char id[64], name[160], key[16] = "";
    snprintf(id, sizeof id, "%s", str(dev, "id"));
    snprintf(name, sizeof name, "%s", str(dev, "name"));
    bool paired = flag(dev, "paired");
    cJSON_Delete(reply);

    bool shown = false;
    double start = kr_now();
    while (!paired && kr_now() - start < PAIR_WAIT_S) {
        cJSON *d = lookup(id);
        if (!d)
            break;
        paired = flag(d, "paired");
        const char *state = str(d, "state");
        if (!shown && *str(d, "key")) {
            snprintf(key, sizeof key, "%s", str(d, "key"));
            printf(_("Verification key: %s\nCheck that %s shows the same key and accept "
                     "there.\n"),
                   key, name);
            fflush(stdout);
            shown = true;
        }
        bool over = strcmp(state, "none") == 0;
        cJSON_Delete(d);
        if (paired || over)
            break;
        sleep_ms(250);
    }
    if (paired) {
        printf(_("Paired with %s.\n"), name);
        return 0;
    }
    kr_log_error(_("pairing with %s did not complete (refused, canceled or timed out; see the "
                   "daemon's log)"),
                 name);
    return 1;
}

int main(int argc, char **argv)
{
    kr_i18n_init();
    kr_log_init("kiremote-cli");

    enum { O_PAIR = 256, O_UNPAIR, O_PING, O_PING_MSG, O_SHARE, O_REFRESH };
    static const struct option longopts[] = {
        {"list", no_argument, NULL, 'l'},
        {"device", required_argument, NULL, 'd'},
        {"pair", no_argument, NULL, O_PAIR},
        {"unpair", no_argument, NULL, O_UNPAIR},
        {"ping", no_argument, NULL, O_PING},
        {"ping-msg", required_argument, NULL, O_PING_MSG},
        {"share", required_argument, NULL, O_SHARE},
        {"refresh", no_argument, NULL, O_REFRESH},
        {"socket", required_argument, NULL, 's'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *device = NULL, *ping_msg = NULL;
    bool do_list = false, do_pair = false, do_unpair = false, do_ping = false, do_refresh = false;
    cJSON *files = cJSON_CreateArray();
    int c;
    while ((c = getopt_long(argc, argv, "ld:s:Vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'l':
            do_list = true;
            break;
        case 'd':
            device = optarg;
            break;
        case O_PAIR:
            do_pair = true;
            break;
        case O_UNPAIR:
            do_unpair = true;
            break;
        case O_PING:
            do_ping = true;
            break;
        case O_PING_MSG:
            do_ping = true;
            ping_msg = optarg;
            break;
        case O_SHARE: {
            char abs[PATH_MAX];
            if (!realpath(optarg, abs)) {
                kr_log_error(_("cannot send %s: %s"), optarg, strerror(errno));
                cJSON_Delete(files);
                return 1;
            }
            cJSON_AddItemToArray(files, cJSON_CreateString(abs));
            break;
        }
        case O_REFRESH:
            do_refresh = true;
            break;
        case 's':
            socket_path = optarg;
            break;
        case 'V':
            printf("kiremote-cli %s\n", KR_VERSION);
            cJSON_Delete(files);
            return 0;
        case 'h':
            usage(stdout);
            cJSON_Delete(files);
            return 0;
        default:
            usage(stderr);
            cJSON_Delete(files);
            return 2;
        }
    }
    bool per_device = do_pair || do_unpair || do_ping || cJSON_GetArraySize(files);
    if (optind < argc || (!per_device && !do_list && !do_refresh) || (per_device && !device)) {
        if (per_device && !device)
            kr_log_error(_("say which device with -d"));
        usage(stderr);
        cJSON_Delete(files);
        return 2;
    }

    char def[KR_PATH_MAX];
    if (!socket_path) {
        if (!kr_control_default_path(def, sizeof def)) {
            cJSON_Delete(files);
            return 1;
        }
        socket_path = def;
    }

    int rc = 0;
    if (do_refresh) {
        cJSON *r = request(simple("refresh", NULL));
        rc |= !r;
        cJSON_Delete(r);
    }
    if (do_unpair) {
        cJSON *r = request(simple("unpair", device));
        rc |= !r;
        cJSON_Delete(r);
    }
    if (do_pair)
        rc |= pair(device);
    if (do_ping && !rc) {
        cJSON *req = simple("ping", device);
        if (ping_msg)
            cJSON_AddStringToObject(req, "message", ping_msg);
        cJSON *r = request(req);
        rc |= !r;
        cJSON_Delete(r);
    }
    if (cJSON_GetArraySize(files) && !rc) {
        cJSON *req = simple("share", device);
        cJSON_AddItemToObject(req, "paths", files);
        files = NULL;
        cJSON *r = request(req);
        rc |= !r;
        cJSON_Delete(r);
    }
    if (do_list)
        rc |= list();
    cJSON_Delete(files);
    return rc;
}
