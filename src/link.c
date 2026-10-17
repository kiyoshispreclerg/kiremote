/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The handshake follows kdeconnect-kde's core/backends/lan/lanlinkprovider.cpp
 * (SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>,
 * GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL). */
#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cert.h"
#include "conn.h"
#include "i18n.h"
#include "log.h"

typedef enum {
    PHASE_CONNECT,  /* outgoing: waiting for TCP */
    PHASE_PLAIN_IN, /* incoming: waiting for their connection identity */
    PHASE_TLS,      /* handshake */
    PHASE_IDENTITY, /* waiting for their full identity */
    PHASE_READY,
    PHASE_DEAD,
} phase;

struct kr_link {
    const kr_link_env *env;
    kr_conn *conn;
    phase ph;
    bool outgoing, connect_failed;
    char id[KR_ID_SIZE];
    int proto;
    double deadline;
    kr_packet *identity;
    X509 *cert;
    char why[256];
};

static void connected(kr_link *l);

static kr_link *link_new(const kr_link_env *env, kr_conn *conn, phase ph, double now)
{
    kr_link *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->env = env;
    l->conn = conn;
    l->ph = ph;
    l->deadline = now + env->timeout;
    return l;
}

kr_link *kr_link_connect(const kr_link_env *env, const struct sockaddr_in *addr,
                         const char *id, int proto, double now)
{
    kr_conn *c = kr_conn_connect(addr);
    if (!c)
        return NULL;
    kr_link *l = link_new(env, c, PHASE_CONNECT, now);
    if (!l) {
        kr_conn_free(c);
        return NULL;
    }
    l->outgoing = true;
    snprintf(l->id, sizeof l->id, "%s", id);
    l->proto = proto;
    return l;
}

kr_link *kr_link_accept(const kr_link_env *env, int fd, double now)
{
    kr_conn *c = kr_conn_wrap(fd);
    if (!c) {
        close(fd);
        return NULL;
    }
    kr_link *l = link_new(env, c, PHASE_PLAIN_IN, now);
    if (!l)
        kr_conn_free(c);
    return l;
}

kr_link *kr_link_from_fd(const kr_link_env *env, int fd, const char *id, int proto,
                         double now)
{
    if (!id)
        return kr_link_accept(env, fd, now);
    kr_conn *c = kr_conn_wrap(fd);
    if (!c) {
        close(fd);
        return NULL;
    }
    kr_link *l = link_new(env, c, PHASE_CONNECT, now);
    if (!l) {
        kr_conn_free(c);
        return NULL;
    }
    l->outgoing = true;
    snprintf(l->id, sizeof l->id, "%s", id);
    l->proto = proto;
    connected(l);
    return l;
}

void kr_link_free(kr_link *l)
{
    if (!l)
        return;
    kr_conn_free(l->conn);
    kr_packet_free(l->identity);
    X509_free(l->cert);
    free(l);
}

kr_packet *kr_link_connection_identity(const kr_packet *identity, const char *target_id,
                                       int target_proto)
{
    kr_packet *p = kr_packet_new(KR_PACKET_IDENTITY);
    if (!p)
        return NULL;
    /* deviceName is unused but older implementations want it. */
    static const char *const keys[] = {"deviceId", "deviceName", "protocolVersion"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(identity->body, keys[i]);
        if (v)
            cJSON_AddItemToObject(p->body, keys[i], cJSON_Duplicate(v, true));
    }
    cJSON_AddStringToObject(p->body, "targetDeviceId", target_id);
    cJSON_AddNumberToObject(p->body, "targetProtocolVersion", target_proto);
    return p;
}

static bool send_packet(kr_conn *c, const kr_packet *p)
{
    size_t len;
    char *s = kr_packet_serialize(p, &len);
    bool ok = s && kr_conn_write(c, s, len);
    free(s);
    return ok;
}

static void die(kr_link *l, const char *why)
{
    if (l->ph == PHASE_DEAD)
        return;
    l->ph = PHASE_DEAD;
    snprintf(l->why, sizeof l->why, "%s", why);
    kr_conn_close(l->conn);
}

/* An identity line, at most KR_MAX_IDENTITY_SIZE bytes. NULL if not
 * complete yet; dies if too long. */
static char *identity_line(kr_link *l, size_t *len)
{
    char *line = kr_conn_line(l->conn, len);
    size_t have;
    kr_conn_data(l->conn, &have);
    if ((line && *len > KR_MAX_IDENTITY_SIZE) || (!line && have > KR_MAX_IDENTITY_SIZE)) {
        die(l, _("identity packet too large"));
        return NULL;
    }
    return line;
}

/* Their plain-text connection identity (we accepted the TCP). */
static void read_connection_identity(kr_link *l)
{
    size_t len;
    char *line = identity_line(l, &len);
    if (!line)
        return;
    kr_packet *p = kr_packet_parse(line, len);
    if (!p || !kr_identity_valid(p, KR_IDENTITY_CONNECTION)) {
        kr_packet_free(p);
        die(l, _("invalid identity packet"));
        return;
    }
    const char *my_id = kr_packet_str(l->env->identity, "deviceId");
    const char *id = kr_packet_str(p, "deviceId");
    int64_t proto = 0, target_proto = KR_PROTOCOL_VERSION;
    kr_packet_int(p, "protocolVersion", &proto);
    if (cJSON_GetObjectItemCaseSensitive(p->body, "targetProtocolVersion") &&
        !kr_packet_int(p, "targetProtocolVersion", &target_proto))
        target_proto = 0;

    const char *err = NULL;
    if (strcmp(kr_packet_str(p, "targetDeviceId"), my_id) != 0)
        err = _("connection meant for another device");
    else if (target_proto != KR_PROTOCOL_VERSION)
        err = _("connection meant for another protocol version");
    else if (proto < KR_PROTOCOL_VERSION)
        err = _("protocol version too old");
    else if (kr_trust_has(l->env->trust, id) && kr_trust_protocol(l->env->trust, id) > proto)
        err = _("refusing a protocol downgrade");
    if (err) {
        kr_packet_free(p);
        die(l, err);
        return;
    }
    snprintf(l->id, sizeof l->id, "%s", id);
    l->proto = (int)proto;
    kr_packet_free(p);

    /* We accepted the TCP connection, so we are the TLS client. */
    l->ph = PHASE_TLS;
    if (!kr_conn_start_tls(l->conn, l->env->ctx, false))
        die(l, _("TLS handshake failed"));
}

/* TLS is up: is this the device it claims to be? */
static void check_peer(kr_link *l)
{
    X509 *peer = kr_conn_peer_cert(l->conn);
    char cn[KR_ID_SIZE];
    if (!peer) {
        die(l, _("the other side sent no certificate"));
        return;
    }
    if (!kr_x509_cn(peer, cn, sizeof cn) || strcmp(cn, l->id) != 0) {
        die(l, _("the certificate does not belong to that device id"));
        return;
    }
    X509 *pinned = kr_trust_cert(l->env->trust, l->id);
    if (pinned && !kr_x509_equal(peer, pinned)) {
        die(l, _("the certificate differs from the one paired"));
        return;
    }
    if (X509_up_ref(peer) != 1) {
        die(l, _("out of memory"));
        return;
    }
    l->cert = peer;
    l->ph = PHASE_IDENTITY;
    if (!send_packet(l->conn, l->env->identity))
        die(l, _("cannot send the identity"));
}

/* Their full identity, inside TLS. */
static void read_full_identity(kr_link *l)
{
    size_t len;
    char *line = identity_line(l, &len);
    if (!line)
        return;
    kr_packet *p = kr_packet_parse(line, len);
    int64_t proto = 0;
    if (!p || !kr_identity_valid(p, KR_IDENTITY_FULL)) {
        kr_packet_free(p);
        die(l, _("invalid identity packet"));
        return;
    }
    kr_packet_int(p, "protocolVersion", &proto);
    if (proto != l->proto || strcmp(kr_packet_str(p, "deviceId"), l->id) != 0) {
        kr_packet_free(p);
        die(l, _("the identity changed during the handshake"));
        return;
    }
    l->identity = p;
    l->ph = PHASE_READY;
    l->deadline = 0;
}

/* We opened the TCP connection, so we are the TLS server. */
static void connected(kr_link *l)
{
    kr_packet *p = kr_link_connection_identity(l->env->identity, l->id, l->proto);
    bool ok = p && send_packet(l->conn, p);
    kr_packet_free(p);
    l->ph = PHASE_TLS;
    if (!ok || !kr_conn_start_tls(l->conn, l->env->ctx, true))
        die(l, _("TLS handshake failed"));
}

int kr_link_pump(kr_link *l, short revents, double now, const char **why)
{
    int out = 0;
    if (l->ph != PHASE_DEAD) {
        const char *cwhy = NULL;
        int ev = kr_conn_pump(l->conn, revents, &cwhy);

        if ((ev & KR_CONN_EV_CONNECTED) && l->ph == PHASE_CONNECT)
            connected(l);
        if (l->ph == PHASE_PLAIN_IN)
            read_connection_identity(l);
        if (l->ph == PHASE_TLS && kr_conn_get_state(l->conn) == KR_CONN_OPEN)
            check_peer(l);
        if (l->ph == PHASE_IDENTITY) {
            read_full_identity(l);
            if (l->ph == PHASE_READY)
                out |= KR_LINK_EV_READY;
        }

        if (l->ph != PHASE_DEAD && (ev & KR_CONN_EV_CLOSED)) {
            if (l->ph == PHASE_CONNECT)
                l->connect_failed = true;
            die(l, cwhy ? cwhy : _("connection closed"));
        }
        if (l->ph != PHASE_DEAD && l->ph != PHASE_READY && now >= l->deadline)
            die(l, _("the handshake timed out"));
    }
    /* Also when kr_link_next killed it. */
    if (l->ph == PHASE_DEAD)
        out |= KR_LINK_EV_CLOSED;
    if (why)
        *why = l->why;
    return out;
}

bool kr_link_pollfd(const kr_link *l, struct pollfd *p)
{
    return l->ph != PHASE_DEAD && kr_conn_pollfd(l->conn, p);
}

double kr_link_deadline(const kr_link *l)
{
    return l->ph == PHASE_READY || l->ph == PHASE_DEAD ? 0 : l->deadline;
}

bool kr_link_ready(const kr_link *l)
{
    return l->ph == PHASE_READY;
}

bool kr_link_outgoing(const kr_link *l)
{
    return l->outgoing;
}

bool kr_link_connect_failed(const kr_link *l)
{
    return l->connect_failed;
}

const char *kr_link_device_id(const kr_link *l)
{
    return l->id;
}

int kr_link_protocol(const kr_link *l)
{
    return l->proto;
}

const struct sockaddr_in *kr_link_peer(const kr_link *l)
{
    return kr_conn_peer(l->conn);
}

const kr_packet *kr_link_identity(const kr_link *l)
{
    return l->identity;
}

X509 *kr_link_cert(const kr_link *l)
{
    return l->cert;
}

kr_packet *kr_link_next(kr_link *l)
{
    if (!l->identity)
        return NULL;
    for (;;) {
        size_t len;
        char *line = kr_conn_line(l->conn, &len);
        if (!line) {
            size_t have;
            kr_conn_data(l->conn, &have);
            if (have > KR_MAX_PACKET_SIZE)
                die(l, _("packet too large"));
            return NULL;
        }
        if (len > KR_MAX_PACKET_SIZE) {
            die(l, _("packet too large"));
            return NULL;
        }
        if (len == 0 || (len == 1 && line[0] == '\r'))
            continue;
        kr_packet *p = kr_packet_parse(line, len);
        if (p)
            return p;
        kr_log_debug("%s: skipping an unparsable packet", l->id);
    }
}

bool kr_link_send(kr_link *l, const kr_packet *p)
{
    return l->ph == PHASE_READY && send_packet(l->conn, p);
}
