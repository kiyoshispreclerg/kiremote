/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Discovery and link management follow kdeconnect-kde's
 * core/backends/lan/lanlinkprovider.cpp and core/device.cpp
 * (SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>,
 * GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL). */
#include "daemon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cert.h"
#include "conn.h"
#include "control.h"
#include "i18n.h"
#include "link.h"
#include "log.h"
#include "net.h"
#include "pair.h"
#include "plugin.h"
#include "transfer.h"
#include "trust.h"
#include "util.h"

#define KR_MAX_PLUGINS 24

/* lanlinkprovider.cpp */
#define MAX_UNPAIRED_LINKS 42
#define SAME_DEVICE_DELAY_S 0.5 /* MILLIS_DELAY_BETWEEN_CONNECTIONS_TO_SAME_DEVICE */
#define MAX_SEEN 1024           /* MAX_TRACKED_CONNECTION_TIMES */
/* Android gives each handshake step 10 s (setSoTimeout). */
#define HANDSHAKE_TIMEOUT_S 10.0
/* Ours: links still in their handshake, at most. */
#define MAX_PENDING 64
/* After a network change, wait for the addresses to settle. */
#define ANNOUNCE_DELAY_S 1.0
/* Two links to the same device this close together are a race where
 * both sides connected at once; see docs/UPSTREAM.md. */
#define LINK_RACE_S 5.0

struct kr_device {
    char id[KR_ID_SIZE];
    char name[KR_NAME_SIZE];
    char type[16];
    int proto;
    cJSON *incoming; /* the device's incomingCapabilities */
    X509 *cert;      /* reference: from the link, or the trust store */
    kr_link *link;   /* ready link, or NULL */
    double link_since;
    short revents;
    kr_pair pair;
    bool up; /* the plugins were told it is connected */
    void *state[KR_MAX_PLUGINS];
};

typedef struct {
    kr_link *link;
    short revents;
} pending;

typedef struct {
    kr_transfer *t;
    char device[KR_NAME_SIZE];
    short revents;
} xfer;

typedef struct {
    char id[KR_ID_SIZE];
    double at;
} seen;

struct kr_daemon {
    const kr_config *cfg;
    kr_cert *cert;
    SSL_CTX *ctx;
    kr_packet *identity;
    kr_trust *trust;
    kr_link_env env;
    kr_control *control;

    kr_bus *bus;
    kr_notify *notify;
    kr_procs *procs;
    kr_mpris *mpris;
    struct src {
        kr_source ops;
        void *ud;
        int first, count; /* its slice of the pollfds of the last turn */
    } *srcs;
    int n_srcs, cap_srcs;

    int udp, tcp, nl;
    int tcp_port;
    double announce_at; /* 0: none scheduled */

    pending *pend;
    int n_pend, cap_pend;
    kr_device **dev;
    int n_dev, cap_dev;
    xfer *xf;
    int n_xf, cap_xf;
    seen seen[MAX_SEEN];
    int n_seen;

    /* What each pollfd of the last kr_daemon_pollfds was. */
    struct slot {
        enum { S_UDP, S_TCP, S_NL, S_CONTROL, S_PEND, S_DEV, S_XFER, S_SRC } kind;
        void *ptr;
    } *slots;
    int n_slots, cap_slots;
    bool flush_now; /* something closed without a pollfd: pump again soon */
};

static bool grow(void **v, int *cap, int need, size_t elem)
{
    if (need <= *cap)
        return true;
    int n = *cap ? *cap * 2 : 8;
    while (n < need)
        n *= 2;
    void *nv = realloc(*v, (size_t)n * elem);
    if (!nv)
        return false;
    *v = nv;
    *cap = n;
    return true;
}

static int64_t wall_now(void)
{
    return (int64_t)time(NULL);
}

/* ---- devices ------------------------------------------------------------- */

const char *kr_device_id(const kr_device *dev)
{
    return dev->id;
}

const char *kr_device_name(const kr_device *dev)
{
    return dev->name;
}

bool kr_device_paired(const kr_device *dev)
{
    return dev->pair.state == KR_PAIR_PAIRED;
}

bool kr_device_reachable(const kr_device *dev)
{
    return dev->link != NULL;
}

const kr_config *kr_daemon_config(const kr_daemon *d)
{
    return d->cfg;
}

static kr_device *find_device(const kr_daemon *d, const char *id)
{
    for (int i = 0; i < d->n_dev; i++)
        if (strcmp(d->dev[i]->id, id) == 0)
            return d->dev[i];
    return NULL;
}

static kr_device *add_device(kr_daemon *d, const char *id)
{
    if (!grow((void **)&d->dev, &d->cap_dev, d->n_dev + 1, sizeof *d->dev))
        return NULL;
    kr_device *dev = calloc(1, sizeof *dev);
    if (!dev)
        return NULL;
    snprintf(dev->id, sizeof dev->id, "%s", id);
    snprintf(dev->name, sizeof dev->name, "%s", id);
    snprintf(dev->type, sizeof dev->type, "phone");
    kr_pair_init(&dev->pair, false);
    d->dev[d->n_dev++] = dev;
    return dev;
}

static void free_device(kr_device *dev)
{
    for (int i = 0; i < KR_MAX_PLUGINS; i++)
        free(dev->state[i]);
    kr_link_free(dev->link);
    X509_free(dev->cert);
    cJSON_Delete(dev->incoming);
    free(dev);
}

static void set_cert(kr_device *dev, X509 *cert)
{
    if (cert && X509_up_ref(cert) == 1) {
        X509_free(dev->cert);
        dev->cert = cert;
    }
}

/* The verification key while pairing, else "". */
static void verification_key(const kr_daemon *d, const kr_device *dev, char out[9])
{
    out[0] = '\0';
    if (kr_pair_deadline(&dev->pair) && dev->cert &&
        !kr_verification_key(kr_cert_x509(d->cert), dev->cert, dev->pair.timestamp, out))
        out[0] = '\0';
}

/* ---- plugins ------------------------------------------------------------- */

static int plugin_index(const kr_plugin *pl)
{
    for (int i = 0; kr_plugins[i]; i++)
        if (kr_plugins[i] == pl)
            return i;
    return -1;
}

void *kr_device_state(kr_device *dev, const kr_plugin *pl)
{
    int i = plugin_index(pl);
    return dev->up && i >= 0 && i < KR_MAX_PLUGINS ? dev->state[i] : NULL;
}

static bool plugin_on(const kr_daemon *d, const kr_plugin *pl)
{
    return kr_config_plugin_enabled(d->cfg, pl->name);
}

/* A paired device is reachable: tell the plugins, once. */
static void device_up(kr_daemon *d, kr_device *dev)
{
    if (dev->up || !dev->link || dev->pair.state != KR_PAIR_PAIRED)
        return;
    dev->up = true;
    for (int i = 0; i < KR_MAX_PLUGINS && kr_plugins[i]; i++) {
        const kr_plugin *pl = kr_plugins[i];
        if (!plugin_on(d, pl))
            continue;
        if (pl->state_size)
            dev->state[i] = calloc(1, pl->state_size);
        if (pl->on_connect && (!pl->state_size || dev->state[i]))
            pl->on_connect(d, dev);
    }
}

/* It is gone (or unpaired): tell the plugins, once. */
static void device_down(kr_daemon *d, kr_device *dev)
{
    if (!dev->up)
        return;
    for (int i = 0; i < KR_MAX_PLUGINS && kr_plugins[i]; i++) {
        const kr_plugin *pl = kr_plugins[i];
        if (plugin_on(d, pl) && pl->on_disconnect && (!pl->state_size || dev->state[i]))
            pl->on_disconnect(d, dev);
        free(dev->state[i]);
        dev->state[i] = NULL;
    }
    dev->up = false;
}

kr_device *kr_daemon_find_device(kr_daemon *d, const char *id)
{
    return find_device(d, id);
}

int kr_daemon_device_count(const kr_daemon *d)
{
    int n = 0;
    for (int i = 0; i < d->n_dev; i++)
        n += d->dev[i]->up;
    return n;
}

kr_device *kr_daemon_device_at(const kr_daemon *d, int k)
{
    for (int i = 0; i < d->n_dev; i++)
        if (d->dev[i]->up && k-- == 0)
            return d->dev[i];
    return NULL;
}

kr_bus *kr_daemon_bus(kr_daemon *d)
{
    return d->bus;
}

kr_notify *kr_daemon_notify(kr_daemon *d)
{
    return d->notify;
}

kr_procs *kr_daemon_procs(kr_daemon *d)
{
    return d->procs;
}

kr_mpris *kr_daemon_mpris(kr_daemon *d)
{
    if (!d->mpris && kr_bus_alive(d->bus))
        d->mpris = kr_mpris_new(d->bus);
    return d->mpris;
}

/* ---- sending ------------------------------------------------------------- */

static bool send_raw(kr_device *dev, const kr_packet *p)
{
    return dev->link && kr_link_send(dev->link, p);
}

static bool takes(const kr_device *dev, const char *type)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, dev->incoming)
    {
        if (cJSON_IsString(e) && strcmp(e->valuestring, type) == 0)
            return true;
    }
    return false;
}

bool kr_daemon_send(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)d;
    if (!kr_device_paired(dev) || !dev->link)
        return false;
    if (!takes(dev, p->type)) {
        kr_log_debug("%s does not take %s", dev->name, p->type);
        return false;
    }
    return send_raw(dev, p);
}

static void send_pair(kr_device *dev, kr_pair_send what)
{
    if (what == KR_PAIR_SEND_NOTHING)
        return;
    kr_packet *p = kr_packet_new(KR_PACKET_PAIR);
    if (!p)
        return;
    cJSON_AddBoolToObject(p->body, "pair", what != KR_PAIR_SEND_REFUSE);
    if (what == KR_PAIR_SEND_REQUEST)
        cJSON_AddNumberToObject(p->body, "timestamp", (double)dev->pair.timestamp);
    send_raw(dev, p);
    kr_packet_free(p);
}

static const char *pair_why(kr_pair_why why)
{
    switch (why) {
    case KR_PAIR_WHY_CLOCKS:
        return _("the clocks of the two devices are too far apart");
    case KR_PAIR_WHY_CANCELED_BY_PEER:
        return _("refused or canceled by the other device");
    case KR_PAIR_WHY_CANCELED_BY_USER:
        return _("canceled");
    case KR_PAIR_WHY_TIMED_OUT:
        return _("timed out");
    case KR_PAIR_WHY_UNREACHABLE:
        return _("the device is not reachable");
    case KR_PAIR_WHY_NONE:
        break;
    }
    return "?";
}

/* Does what the pairing state machine said. */
static void apply_pair(kr_daemon *d, kr_device *dev, kr_pair_out out)
{
    send_pair(dev, out.send);
    if (out.events & KR_PAIR_EV_UNPAIRED) {
        device_down(d, dev);
        kr_trust_remove(d->trust, dev->id);
        kr_log_info(_("unpaired from %s"), dev->name);
    }
    if (out.events & KR_PAIR_EV_FAILED)
        kr_log_info(_("pairing with %s failed: %s"), dev->name, pair_why(out.why));
    if (out.events & KR_PAIR_EV_INCOMING) {
        char key[9];
        verification_key(d, dev, key);
        kr_log_info(_("%s wants to pair; verification key %s. "
                      "Accept with \"kiremote-cli -d %s --pair\", refuse with \"--unpair\"."),
                    dev->name, key, dev->id);
    }
    if (out.events & KR_PAIR_EV_PAIRED) {
        if (!dev->cert || !kr_trust_add(d->trust, dev->id, dev->cert, dev->name, dev->type, dev->proto))
            kr_log_error(_("paired with %s, but could not save it"), dev->name);
        else
            kr_log_info(_("paired with %s"), dev->name);
        device_up(d, dev);
    }
}

/* ---- packets ------------------------------------------------------------- */

static void handle_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    if (strcmp(p->type, KR_PACKET_PAIR) == 0) {
        int64_t ts = 0;
        bool has_ts = kr_packet_int(p, "timestamp", &ts);
        apply_pair(d, dev, kr_pair_receive(&dev->pair, kr_packet_bool(p, "pair", false), has_ts,
                                           ts, wall_now(), kr_now()));
        return;
    }
    if (strcmp(p->type, KR_PACKET_IDENTITY) == 0)
        return;
    if (!kr_device_paired(dev)) {
        /* It thinks we are paired; we don't (device.cpp: unpair()). */
        kr_log_debug("%s is not paired, ignoring %s", dev->name, p->type);
        send_pair(dev, KR_PAIR_SEND_REFUSE);
        return;
    }
    bool taken = false;
    for (int i = 0; kr_plugins[i]; i++) {
        const kr_plugin *pl = kr_plugins[i];
        if (plugin_on(d, pl) && pl->on_packet && kr_plugin_takes(pl, p->type)) {
            taken = true;
            if (dev->up)
                pl->on_packet(d, dev, p);
        }
    }
    if (!taken)
        kr_log_debug("%s: no plugin takes %s", dev->name, p->type);
}

/* ---- links --------------------------------------------------------------- */

static int unpaired_links(const kr_daemon *d)
{
    int n = 0;
    for (int i = 0; i < d->n_dev; i++)
        n += d->dev[i]->link && !kr_trust_has(d->trust, d->dev[i]->id);
    return n;
}

/* Who opened the TCP connection of l. */
static const char *initiator(const kr_daemon *d, const kr_device *dev, const kr_link *l)
{
    return kr_link_outgoing(l) ? kr_cert_device_id(d->cert) : dev->id;
}

/* A handshake finished: the link goes to its device. False if refused
 * (the caller frees it). */
static bool adopt(kr_daemon *d, kr_link *l)
{
    const char *id = kr_link_device_id(l);
    const kr_packet *ident = kr_link_identity(l);
    kr_device *dev = find_device(d, id);

    if (dev && dev->link) {
        if (!kr_x509_equal(dev->cert, kr_link_cert(l))) {
            kr_log_warn(_("%s connected again with another certificate; refused"), dev->name);
            return false;
        }
        /* Both sides connected at once: keep, on both sides, the link the
         * larger id opened. Otherwise the new one replaces the old. */
        if (kr_now() - dev->link_since < LINK_RACE_S &&
            strcmp(initiator(d, dev, dev->link), initiator(d, dev, l)) > 0) {
            kr_log_debug("%s: keeping the earlier link", dev->name);
            return false;
        }
        device_down(d, dev);
        kr_link_free(dev->link);
        dev->link = NULL;
    } else if (!kr_trust_has(d->trust, id) && unpaired_links(d) >= MAX_UNPAIRED_LINKS) {
        kr_log_warn(_("too many unpaired devices around; ignoring %s"), id);
        return false;
    }

    if (!dev && !(dev = add_device(d, id)))
        return false;
    char name[KR_NAME_SIZE];
    kr_name_filter(kr_packet_str(ident, "deviceName"), name, sizeof name);
    snprintf(dev->name, sizeof dev->name, "%s", name);
    const char *type = kr_packet_str(ident, "deviceType");
    snprintf(dev->type, sizeof dev->type, "%s",
             type && kr_device_type_valid(type) ? strcmp(type, "smartphone") == 0 ? "phone" : type
                                                : "desktop");
    dev->proto = kr_link_protocol(l);
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(ident->body, "incomingCapabilities");
    const cJSON *out = cJSON_GetObjectItemCaseSensitive(ident->body, "outgoingCapabilities");
    /* As updateDeviceInfo: only when both lists came. */
    if (cJSON_IsArray(in) && cJSON_GetArraySize(in) && cJSON_IsArray(out) && cJSON_GetArraySize(out)) {
        cJSON_Delete(dev->incoming);
        dev->incoming = cJSON_Duplicate(in, true);
    }
    set_cert(dev, kr_link_cert(l));
    dev->link = l;
    dev->link_since = kr_now();
    dev->revents = 0;
    kr_trust_update(d->trust, id, dev->name, dev->type, dev->proto);

    char addr[INET_ADDRSTRLEN];
    kr_log_info(_("connected to %s (%s, %s)"), dev->name, dev->type,
                kr_addr_str(&kr_link_peer(l)->sin_addr, addr, sizeof addr));
    device_up(d, dev);
    return true;
}

static void send_discovery(kr_daemon *d, const struct in_addr *to)
{
    kr_packet *p = kr_packet_new(KR_PACKET_IDENTITY);
    if (!p)
        return;
    static const char *const keys[] = {"deviceId", "deviceName", "protocolVersion"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        cJSON_AddItemToObject(p->body, keys[i],
                              cJSON_Duplicate(cJSON_GetObjectItem(d->identity->body, keys[i]), true));
    cJSON_AddNumberToObject(p->body, "tcpPort", d->tcp_port);
    size_t len;
    char *s = kr_packet_serialize(p, &len);
    kr_packet_free(p);
    if (!s)
        return;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(KR_UDP_PORT), .sin_addr = *to};
    char buf[INET_ADDRSTRLEN];
    if (sendto(d->udp, s, len, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof sa) < 0)
        kr_log_debug("discovery to %s: %s", kr_addr_str(to, buf, sizeof buf), strerror(errno));
    else
        kr_log_debug("discovery sent to %s", kr_addr_str(to, buf, sizeof buf));
    free(s);
}

void kr_daemon_announce(kr_daemon *d)
{
    d->announce_at = 0;
    struct in_addr addrs[32];
    int n = kr_broadcast_addrs(addrs, 32);
    for (int i = 0; i < n; i++)
        send_discovery(d, &addrs[i]);

    const char *list = kr_config_addresses(d->cfg);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", list ? list : "");
    for (char *tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t")) {
        struct in_addr a;
        if (inet_pton(AF_INET, tok, &a) == 1)
            send_discovery(d, &a);
        else
            kr_log_warn(_("[network] addresses: not an IPv4 address: %s"), tok);
    }
}

/* Rate limit per device id (deviceDiscovered upstream). True if id may
 * be connected to now. */
static bool may_connect(kr_daemon *d, const char *id, double now)
{
    int k = 0;
    for (int i = 0; i < d->n_seen; i++)
        if (d->seen[i].at + SAME_DEVICE_DELAY_S > now)
            d->seen[k++] = d->seen[i];
    d->n_seen = k;
    for (int i = 0; i < d->n_seen; i++)
        if (strcmp(d->seen[i].id, id) == 0)
            return false;
    if (d->n_seen == MAX_SEEN)
        return false;
    snprintf(d->seen[d->n_seen].id, sizeof d->seen[0].id, "%s", id);
    d->seen[d->n_seen++].at = now;
    return true;
}

static void add_pending(kr_daemon *d, kr_link *l)
{
    if (!l)
        return;
    if (d->n_pend >= MAX_PENDING ||
        !grow((void **)&d->pend, &d->cap_pend, d->n_pend + 1, sizeof *d->pend)) {
        kr_link_free(l);
        return;
    }
    d->pend[d->n_pend++] = (pending){.link = l};
}

/* Someone announced itself on UDP: connect to it. */
static void on_discovery(kr_daemon *d, const char *data, size_t len, const struct sockaddr_in *from)
{
    char addr[INET_ADDRSTRLEN];
    kr_addr_str(&from->sin_addr, addr, sizeof addr);
    kr_packet *p = kr_packet_parse(data, len);
    if (!p || !kr_identity_valid(p, KR_IDENTITY_UDP)) {
        kr_log_debug("invalid discovery packet from %s", addr);
        kr_packet_free(p);
        return;
    }
    const char *id = kr_packet_str(p, "deviceId");
    int64_t proto = 0, port = 0;
    kr_packet_int(p, "protocolVersion", &proto);
    kr_packet_int(p, "tcpPort", &port);
    double now = kr_now();

    if (strcmp(id, kr_cert_device_id(d->cert)) == 0 || (ntohl(from->sin_addr.s_addr) >> 24) == 127)
        ; /* our own broadcast, or loopback */
    else if (proto < KR_PROTOCOL_VERSION)
        kr_log_debug("ignoring %s (%s): protocol version %d", id, addr, (int)proto);
    else if (port < KR_TCP_FIRST || port > KR_TCP_LAST)
        kr_log_debug("ignoring %s (%s): TCP port %d", id, addr, (int)port);
    else if (!may_connect(d, id, now))
        kr_log_debug("ignoring %s (%s): discovered again too soon", id, addr);
    else if (kr_trust_has(d->trust, id) && kr_trust_protocol(d->trust, id) > proto)
        kr_log_warn(_("refusing %s: it went back to an older protocol version"), id);
    else {
        kr_log_debug("discovered %s at %s:%d", id, addr, (int)port);
        struct sockaddr_in to = *from;
        to.sin_port = htons((uint16_t)port);
        kr_link *l = kr_link_connect(&d->env, &to, id, (int)proto, now);
        if (l)
            add_pending(d, l);
        else
            send_discovery(d, &from->sin_addr); /* let it try us instead */
    }
    kr_packet_free(p);
}

static void read_udp(kr_daemon *d)
{
    static char buf[65536];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(d->udp, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n < 0)
            return;
        if (from.sin_family == AF_INET)
            on_discovery(d, buf, (size_t)n, &from);
    }
}

static void accept_tcp(kr_daemon *d)
{
    for (;;) {
        int fd = accept(d->tcp, NULL, NULL);
        if (fd < 0)
            return;
        add_pending(d, kr_link_accept(&d->env, fd, kr_now()));
    }
}

/* ---- transfers ----------------------------------------------------------- */

static bool add_transfer(kr_daemon *d, kr_device *dev, kr_transfer *t)
{
    if (!t)
        return false;
    if (!grow((void **)&d->xf, &d->cap_xf, d->n_xf + 1, sizeof *d->xf)) {
        kr_transfer_free(t);
        return false;
    }
    d->xf[d->n_xf] = (xfer){.t = t};
    snprintf(d->xf[d->n_xf].device, sizeof d->xf[0].device, "%s", dev->name);
    d->n_xf++;
    return true;
}

bool kr_daemon_download(kr_daemon *d, kr_device *dev, const kr_packet *p, const char *path,
                        int64_t mtime_ms)
{
    if (!dev->link || !dev->cert || !p->payload_port)
        return false;
    struct sockaddr_in to = *kr_link_peer(dev->link);
    to.sin_port = htons((uint16_t)p->payload_port);
    kr_transfer *t = kr_transfer_download(d->ctx, dev->cert, &to, p->payload_size, path, mtime_ms,
                                          kr_now());
    if (!t) {
        kr_log_warn(_("cannot receive %s: %s"), path, strerror(errno));
        return false;
    }
    return add_transfer(d, dev, t);
}

bool kr_daemon_upload(kr_daemon *d, kr_device *dev, kr_packet *p, int fd, int64_t size,
                      const char *name)
{
    if (!kr_device_paired(dev) || !dev->link || !dev->cert || !takes(dev, p->type)) {
        close(fd);
        return false;
    }
    int port = 0;
    kr_transfer *t = kr_transfer_upload(d->ctx, dev->cert, fd, size, name, kr_now(), &port);
    if (!t) {
        kr_log_warn(_("no free port for sending %s"), name);
        return false;
    }
    kr_packet_set_payload(p, size, port);
    if (!send_raw(dev, p) || !add_transfer(d, dev, t)) {
        kr_transfer_free(t);
        return false;
    }
    kr_log_info(_("sending %s to %s"), name, dev->name);
    return true;
}

/* ---- control requests ---------------------------------------------------- */

static const char *state_name(kr_pair_state s)
{
    switch (s) {
    case KR_PAIR_PAIRED:
        return "paired";
    case KR_PAIR_REQUESTED:
        return "requested";
    case KR_PAIR_REQUESTED_BY_PEER:
        return "requested_by_peer";
    case KR_PAIR_NONE:
        break;
    }
    return "none";
}

static cJSON *device_json(const kr_daemon *d, const kr_device *dev)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "id", dev->id);
    cJSON_AddStringToObject(o, "name", dev->name);
    cJSON_AddStringToObject(o, "type", dev->type);
    cJSON_AddBoolToObject(o, "reachable", dev->link != NULL);
    cJSON_AddBoolToObject(o, "paired", kr_device_paired(dev));
    cJSON_AddStringToObject(o, "state", state_name(dev->pair.state));
    char key[9];
    verification_key(d, dev, key);
    if (key[0])
        cJSON_AddStringToObject(o, "key", key);
    if (dev->link) {
        char addr[INET_ADDRSTRLEN];
        cJSON_AddStringToObject(o, "address",
                                kr_addr_str(&kr_link_peer(dev->link)->sin_addr, addr, sizeof addr));
    }
    for (int i = 0; dev->up && kr_plugins[i]; i++)
        if (kr_plugins[i]->describe && plugin_on(d, kr_plugins[i]))
            kr_plugins[i]->describe((kr_daemon *)d, dev, o);
    return o;
}

/* The device named by req's "device" (an id, or a unique name). */
kr_device *kr_daemon_request_device(kr_daemon *d, const cJSON *req, cJSON *reply)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, "device");
    if (!cJSON_IsString(v)) {
        kr_control_fail(reply, _("missing \"device\""));
        return NULL;
    }
    kr_device *found = find_device(d, v->valuestring);
    for (int i = 0; !find_device(d, v->valuestring) && i < d->n_dev; i++)
        if (strcmp(d->dev[i]->name, v->valuestring) == 0) {
            if (found) {
                kr_control_fail(reply, _("more than one device has that name; use the id"));
                return NULL;
            }
            found = d->dev[i];
        }
    if (!found) {
        char err[256];
        snprintf(err, sizeof err, _("no such device: %s"), v->valuestring);
        kr_control_fail(reply, err);
    }
    return found;
}

static void on_request(void *ud, const char *type, const cJSON *req, cJSON *reply)
{
    kr_daemon *d = ud;
    if (strcmp(type, "version") == 0) {
        cJSON_AddStringToObject(reply, "version", KR_VERSION);
        cJSON_AddStringToObject(reply, "id", kr_cert_device_id(d->cert));
        return;
    }
    if (strcmp(type, "devices") == 0) {
        cJSON *a = cJSON_AddArrayToObject(reply, "devices");
        for (int i = 0; a && i < d->n_dev; i++)
            cJSON_AddItemToArray(a, device_json(d, d->dev[i]));
        return;
    }
    if (strcmp(type, "refresh") == 0) {
        kr_daemon_announce(d);
        return;
    }
    if (strcmp(type, "pair") != 0 && strcmp(type, "unpair") != 0 && strcmp(type, "ping") != 0 &&
        strcmp(type, "share") != 0) {
        for (int i = 0; kr_plugins[i]; i++)
            if (kr_plugins[i]->request && plugin_on(d, kr_plugins[i]) &&
                kr_plugins[i]->request(d, type, req, reply))
                return;
        kr_control_fail(reply, _("unknown request"));
        return;
    }

    kr_device *dev = kr_daemon_request_device(d, req, reply);
    if (!dev)
        return;
    if (strcmp(type, "pair") == 0) {
        if (kr_device_paired(dev)) {
            kr_control_fail(reply, _("already paired"));
            return;
        }
        kr_pair_out out = kr_pair_request(&dev->pair, dev->link != NULL, wall_now(), kr_now());
        apply_pair(d, dev, out);
        if (out.events & KR_PAIR_EV_FAILED)
            kr_control_fail(reply, pair_why(out.why));
    } else if (strcmp(type, "unpair") == 0) {
        apply_pair(d, dev, kr_pair_unpair(&dev->pair));
        kr_trust_remove(d->trust, dev->id);
    } else if (!kr_device_paired(dev)) {
        kr_control_fail(reply, _("the device is not paired"));
        return;
    } else if (!dev->link) {
        kr_control_fail(reply, pair_why(KR_PAIR_WHY_UNREACHABLE));
        return;
    } else if (strcmp(type, "ping") == 0) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "message");
        if (!kr_ping_send(d, dev, cJSON_IsString(m) ? m->valuestring : NULL))
            kr_control_fail(reply, _("the device does not take pings"));
    } else {
        const cJSON *paths = cJSON_GetObjectItemCaseSensitive(req, "paths");
        cJSON *errors = cJSON_CreateArray();
        int sent = kr_share_files(d, dev, paths, errors);
        if (cJSON_GetArraySize(errors)) {
            kr_control_fail(reply, cJSON_GetArrayItem(errors, 0)->valuestring);
            cJSON_AddItemToObject(reply, "errors", errors);
        } else {
            cJSON_Delete(errors);
        }
        cJSON_AddNumberToObject(reply, "sent", sent);
    }
    cJSON *o = device_json(d, dev);
    if (o)
        cJSON_AddItemToObject(reply, "device", o);
}

/* ---- sources ------------------------------------------------------------- */

bool kr_daemon_add_source(kr_daemon *d, const kr_source *s, void *ud)
{
    if (!grow((void **)&d->srcs, &d->cap_srcs, d->n_srcs + 1, sizeof *d->srcs))
        return false;
    d->srcs[d->n_srcs++] = (struct src){.ops = *s, .ud = ud};
    return true;
}

void kr_daemon_remove_source(kr_daemon *d, void *ud)
{
    for (int i = 0; i < d->n_srcs; i++)
        if (d->srcs[i].ud == ud) {
            memmove(&d->srcs[i], &d->srcs[i + 1], (size_t)(d->n_srcs - i - 1) * sizeof *d->srcs);
            d->n_srcs--;
            return;
        }
}

static int procs_pollfds(void *ud, struct pollfd *fds, int max)
{
    return kr_procs_pollfds(ud, fds, max);
}

static void procs_dispatch(void *ud, const struct pollfd *fds, int n)
{
    kr_procs_dispatch(ud, fds, n);
}

static double procs_deadline(void *ud)
{
    return kr_procs_deadline(ud);
}

static int bus_pollfds(void *ud, struct pollfd *fds, int max)
{
    return kr_bus_pollfds(ud, fds, max);
}

static void bus_dispatch(void *ud, const struct pollfd *fds, int n)
{
    kr_bus_dispatch(ud, fds, n);
}

static double bus_deadline(void *ud)
{
    return kr_bus_deadline(ud);
}

/* ---- setup --------------------------------------------------------------- */

static kr_packet *make_identity(const kr_config *cfg, const char *id)
{
    kr_packet *p = kr_packet_new(KR_PACKET_IDENTITY);
    if (!p)
        return NULL;
    char name[KR_NAME_SIZE];
    kr_name_filter(kr_config_device_name(cfg), name, sizeof name);
    if (!name[0])
        snprintf(name, sizeof name, "kiremote");
    cJSON_AddStringToObject(p->body, "deviceId", id);
    cJSON_AddStringToObject(p->body, "deviceName", name);
    cJSON_AddStringToObject(p->body, "deviceType", kr_config_device_type(cfg));
    cJSON_AddNumberToObject(p->body, "protocolVersion", KR_PROTOCOL_VERSION);
    cJSON_AddItemToObject(p->body, "incomingCapabilities", kr_plugins_capabilities(cfg, true));
    cJSON_AddItemToObject(p->body, "outgoingCapabilities", kr_plugins_capabilities(cfg, false));
    return p;
}

kr_daemon *kr_daemon_new(const kr_config *cfg, const char *socket_path)
{
    kr_daemon *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->cfg = cfg;
    d->udp = d->tcp = d->nl = -1;

    char dir[KR_PATH_MAX], trusted[KR_PATH_MAX], sock[KR_PATH_MAX];
    if (!kr_config_path(dir, sizeof dir, "") || !kr_config_path(trusted, sizeof trusted, "trusted")) {
        kr_log_error(_("cannot find the configuration folder (is HOME set?)"));
        goto fail;
    }
    dir[strlen(dir) - 1] = '\0'; /* the trailing "/" */
    bool created = false;
    if (!(d->cert = kr_cert_load_or_create(dir, &created)))
        goto fail;
    if (!(d->ctx = kr_conn_ssl_ctx(kr_cert_x509(d->cert), kr_cert_key(d->cert))) ||
        !(d->identity = make_identity(cfg, kr_cert_device_id(d->cert))) ||
        !(d->trust = kr_trust_open(trusted))) {
        kr_log_error(_("out of memory"));
        goto fail;
    }
    if (created && kr_trust_count(d->trust)) {
        kr_log_warn(_("new certificate: forgetting the old pairings"));
        kr_trust_clear(d->trust);
    }
    d->env = (kr_link_env){.ctx = d->ctx, .identity = d->identity, .trust = d->trust,
                           .timeout = HANDSHAKE_TIMEOUT_S};

    if (!socket_path) {
        if (!kr_control_default_path(sock, sizeof sock))
            goto fail;
        socket_path = sock;
    }
    if (!(d->control = kr_control_open(socket_path, on_request, d)))
        goto fail;

    d->udp = kr_udp_open(KR_UDP_PORT, true);
    if (d->udp < 0) {
        kr_log_warn(_("UDP port %d is taken (is KDE Connect running?): other devices won't be "
                      "discovered, but they can still find us"),
                    KR_UDP_PORT);
        d->udp = kr_udp_open(KR_UDP_PORT, false);
    }
    d->tcp = kr_tcp_listen(KR_TCP_FIRST, KR_TCP_LAST, &d->tcp_port);
    if (d->udp < 0 || d->tcp < 0) {
        kr_log_error(_("cannot open the network ports (%d-%d): %s"), KR_TCP_FIRST, KR_TCP_LAST,
                     strerror(errno));
        goto fail;
    }
    d->nl = kr_netlink_open();

    for (int i = 0; i < kr_trust_count(d->trust); i++) {
        const char *id = kr_trust_id(d->trust, i);
        kr_device *dev = add_device(d, id);
        if (!dev)
            continue;
        snprintf(dev->name, sizeof dev->name, "%s", kr_trust_name(d->trust, id));
        snprintf(dev->type, sizeof dev->type, "%s", kr_trust_type(d->trust, id));
        dev->proto = kr_trust_protocol(d->trust, id);
        set_cert(dev, kr_trust_cert(d->trust, id));
        kr_pair_init(&dev->pair, true);
    }

    /* Services the plugins share. */
    d->procs = kr_procs_new();
    if (d->procs)
        kr_daemon_add_source(d, &(kr_source){procs_pollfds, procs_dispatch, procs_deadline}, d->procs);
    d->bus = kr_bus_open();
    if (d->bus)
        kr_daemon_add_source(d, &(kr_source){bus_pollfds, bus_dispatch, bus_deadline}, d->bus);
    else
        kr_log_info(_("no session bus: notifications and media control are off"));
    d->notify = kr_notify_new(d->bus);
    for (int i = 0; kr_plugins[i]; i++)
        if (kr_plugins[i]->init && plugin_on(d, kr_plugins[i]))
            kr_plugins[i]->init(d);

    kr_log_info(_("this is %s (%s), id %s, TCP port %d"), kr_packet_str(d->identity, "deviceName"),
                kr_config_device_type(cfg), kr_cert_device_id(d->cert), d->tcp_port);
    kr_daemon_announce(d);
    return d;

fail:
    kr_daemon_free(d);
    return NULL;
}

void kr_daemon_free(kr_daemon *d)
{
    if (!d)
        return;
    for (int i = 0; i < d->n_pend; i++)
        kr_link_free(d->pend[i].link);
    for (int i = 0; i < d->n_dev; i++)
        device_down(d, d->dev[i]);
    for (int i = 0; d->procs && kr_plugins[i]; i++)
        if (kr_plugins[i]->shutdown && plugin_on(d, kr_plugins[i]))
            kr_plugins[i]->shutdown(d);
    for (int i = 0; i < d->n_dev; i++)
        free_device(d->dev[i]);
    for (int i = 0; i < d->n_xf; i++)
        kr_transfer_free(d->xf[i].t);
    free(d->pend);
    free(d->dev);
    free(d->xf);
    free(d->slots);
    kr_mpris_free(d->mpris);
    kr_notify_free(d->notify);
    kr_bus_free(d->bus);
    kr_procs_free(d->procs);
    free(d->srcs);
    kr_control_close(d->control);
    if (d->udp >= 0)
        close(d->udp);
    if (d->tcp >= 0)
        close(d->tcp);
    if (d->nl >= 0)
        close(d->nl);
    kr_trust_free(d->trust);
    kr_packet_free(d->identity);
    SSL_CTX_free(d->ctx);
    kr_cert_free(d->cert);
    free(d);
}

/* ---- the loop ------------------------------------------------------------ */

static void add_slot(kr_daemon *d, struct pollfd *fds, int *n, int max, struct pollfd p, int kind,
                     void *ptr)
{
    if (*n >= max || !grow((void **)&d->slots, &d->cap_slots, *n + 1, sizeof *d->slots))
        return;
    fds[*n] = p;
    d->slots[*n] = (struct slot){.kind = kind, .ptr = ptr};
    (*n)++;
}

int kr_daemon_pollfds(kr_daemon *d, struct pollfd *fds, int max)
{
    int n = 0;
    struct pollfd p;
    d->flush_now = false;
    add_slot(d, fds, &n, max, (struct pollfd){.fd = d->udp, .events = POLLIN}, S_UDP, NULL);
    add_slot(d, fds, &n, max, (struct pollfd){.fd = d->tcp, .events = POLLIN}, S_TCP, NULL);
    if (d->nl >= 0)
        add_slot(d, fds, &n, max, (struct pollfd){.fd = d->nl, .events = POLLIN}, S_NL, NULL);
    struct pollfd cfds[32];
    int nc = kr_control_pollfds(d->control, cfds, 32);
    for (int i = 0; i < nc; i++)
        add_slot(d, fds, &n, max, cfds[i], S_CONTROL, NULL);
    for (int i = 0; i < d->n_pend; i++) {
        if (kr_link_pollfd(d->pend[i].link, &p))
            add_slot(d, fds, &n, max, p, S_PEND, d->pend[i].link);
        else
            d->flush_now = true;
    }
    for (int i = 0; i < d->n_dev; i++) {
        if (!d->dev[i]->link)
            continue;
        if (kr_link_pollfd(d->dev[i]->link, &p))
            add_slot(d, fds, &n, max, p, S_DEV, d->dev[i]);
        else
            d->flush_now = true;
    }
    for (int i = 0; i < d->n_xf; i++) {
        if (kr_transfer_pollfd(d->xf[i].t, &p))
            add_slot(d, fds, &n, max, p, S_XFER, d->xf[i].t);
        else
            d->flush_now = true;
    }
    for (int i = 0; i < d->n_srcs; i++) {
        struct src *sr = &d->srcs[i];
        sr->first = n;
        struct pollfd tmp[32];
        int k = sr->ops.pollfds ? sr->ops.pollfds(sr->ud, tmp, 32) : 0;
        for (int j = 0; j < k; j++)
            add_slot(d, fds, &n, max, tmp[j], S_SRC, sr);
        sr->count = n - sr->first;
    }
    d->n_slots = n;
    return n;
}

int kr_daemon_timeout(const kr_daemon *d)
{
    if (d->flush_now)
        return 0;
    double next = 0, now = kr_now();
#define CONSIDER(t)                                                                                \
    do {                                                                                           \
        double t_ = (t);                                                                           \
        if (t_ > 0 && (next == 0 || t_ < next))                                                    \
            next = t_;                                                                             \
    } while (0)
    CONSIDER(d->announce_at);
    for (int i = 0; i < d->n_pend; i++)
        CONSIDER(kr_link_deadline(d->pend[i].link));
    for (int i = 0; i < d->n_dev; i++)
        CONSIDER(kr_pair_deadline(&d->dev[i]->pair));
    for (int i = 0; i < d->n_xf; i++)
        CONSIDER(kr_transfer_deadline(d->xf[i].t));
    for (int i = 0; i < d->n_srcs; i++)
        if (d->srcs[i].ops.deadline)
            CONSIDER(d->srcs[i].ops.deadline(d->srcs[i].ud));
#undef CONSIDER
    if (next == 0)
        return -1;
    double ms = (next - now) * 1000 + 1;
    return ms < 0 ? 0 : ms > 3600000 ? 3600000 : (int)ms;
}

static void pump_pending(kr_daemon *d, double now)
{
    for (int i = 0; i < d->n_pend;) {
        pending *pe = &d->pend[i];
        const char *why = NULL;
        int ev = kr_link_pump(pe->link, pe->revents, now, &why);
        pe->revents = 0;
        if (!(ev & (KR_LINK_EV_READY | KR_LINK_EV_CLOSED))) {
            i++;
            continue;
        }
        kr_link *l = pe->link;
        d->pend[i] = d->pend[--d->n_pend];
        if ((ev & KR_LINK_EV_READY) && adopt(d, l)) {
            if (ev & KR_LINK_EV_CLOSED) /* the device pump reports it */
                d->flush_now = true;
            continue;
        }
        if (ev & KR_LINK_EV_CLOSED) {
            char addr[INET_ADDRSTRLEN];
            kr_log_debug("handshake with %s (%s) failed: %s",
                         kr_link_device_id(l)[0] ? kr_link_device_id(l) : "?",
                         kr_addr_str(&kr_link_peer(l)->sin_addr, addr, sizeof addr), why);
            /* recoverConnectionError: maybe it can reach us instead. */
            if (kr_link_connect_failed(l))
                send_discovery(d, &kr_link_peer(l)->sin_addr);
        }
        kr_link_free(l);
    }
}

static void pump_devices(kr_daemon *d, double now)
{
    for (int i = 0; i < d->n_dev; i++) {
        kr_device *dev = d->dev[i];
        if (dev->link) {
            const char *why = NULL;
            int ev = kr_link_pump(dev->link, dev->revents, now, &why);
            dev->revents = 0;
            kr_packet *p;
            while (dev->link && (p = kr_link_next(dev->link))) {
                handle_packet(d, dev, p);
                kr_packet_free(p);
            }
            if (dev->link && ((ev & KR_LINK_EV_CLOSED) || !kr_link_ready(dev->link))) {
                kr_log_info(_("disconnected from %s: %s"), dev->name, why ? why : "?");
                device_down(d, dev);
                kr_link_free(dev->link);
                dev->link = NULL;
            }
        }
        if (kr_pair_deadline(&dev->pair) && now >= kr_pair_deadline(&dev->pair))
            apply_pair(d, dev, kr_pair_tick(&dev->pair, now));
    }
    /* Forget the devices that are neither trusted, connected nor pairing. */
    for (int i = 0; i < d->n_dev;) {
        kr_device *dev = d->dev[i];
        if (!dev->link && !kr_trust_has(d->trust, dev->id) && !kr_pair_deadline(&dev->pair)) {
            free_device(dev);
            d->dev[i] = d->dev[--d->n_dev];
        } else {
            i++;
        }
    }
}

static void pump_transfers(kr_daemon *d, double now)
{
    for (int i = 0; i < d->n_xf;) {
        xfer *x = &d->xf[i];
        const char *why = NULL;
        int r = kr_transfer_pump(x->t, x->revents, now, &why);
        x->revents = 0;
        if (r == KR_TRANSFER_RUNNING) {
            i++;
            continue;
        }
        if (r == KR_TRANSFER_DONE)
            kr_log_info(kr_transfer_is_upload(x->t) ? _("sent %s to %s") : _("received %s from %s"),
                        kr_transfer_name(x->t), x->device);
        else
            kr_log_warn(kr_transfer_is_upload(x->t) ? _("sending %s to %s failed: %s")
                                                    : _("receiving %s from %s failed: %s"),
                        kr_transfer_name(x->t), x->device, why);
        kr_transfer_free(x->t);
        d->xf[i] = d->xf[--d->n_xf];
    }
}

void kr_daemon_dispatch(kr_daemon *d, const struct pollfd *fds, int n)
{
    double now = kr_now();
    bool udp = false, tcp = false, nl = false;
    /* First note what each object got, while every pointer is valid. */
    for (int i = 0; i < n && i < d->n_slots; i++) {
        if (!fds[i].revents)
            continue;
        struct slot *s = &d->slots[i];
        switch (s->kind) {
        case S_UDP:
            udp = true;
            break;
        case S_TCP:
            tcp = true;
            break;
        case S_NL:
            nl = true;
            break;
        case S_CONTROL:
            break;
        case S_PEND:
            for (int k = 0; k < d->n_pend; k++)
                if (d->pend[k].link == s->ptr)
                    d->pend[k].revents = fds[i].revents;
            break;
        case S_DEV:
            ((kr_device *)s->ptr)->revents = fds[i].revents;
            break;
        case S_XFER:
            for (int k = 0; k < d->n_xf; k++)
                if (d->xf[k].t == s->ptr)
                    d->xf[k].revents = fds[i].revents;
            break;
        case S_SRC:
            break;
        }
    }

    if (udp)
        read_udp(d);
    if (tcp)
        accept_tcp(d);
    if (nl && kr_netlink_changed(d->nl) && !d->announce_at)
        d->announce_at = now + ANNOUNCE_DELAY_S;
    if (d->announce_at && now >= d->announce_at)
        kr_daemon_announce(d);

    pump_pending(d, now);
    pump_devices(d, now);

    /* Control requests last: they may start transfers and send packets. */
    struct pollfd cfds[32];
    int nc = 0;
    for (int i = 0; i < n && i < d->n_slots && nc < 32; i++)
        if (d->slots[i].kind == S_CONTROL)
            cfds[nc++] = fds[i];
    kr_control_dispatch(d->control, cfds, nc);

    pump_transfers(d, now);

    /* Sources last: what they report (a bus reply, a process that ended)
     * may start more work, and they see the devices as the packets left
     * them. A source may add or remove sources, so index, don't hold. */
    for (int i = 0; i < d->n_srcs; i++) {
        struct src sr = d->srcs[i];
        const struct pollfd *mine = fds + sr.first;
        int cnt = sr.first + sr.count <= n ? sr.count : 0;
        if (sr.ops.dispatch)
            sr.ops.dispatch(sr.ud, mine, cnt);
    }
}
