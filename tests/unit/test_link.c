/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The link handshake between two of our links over a socketpair: plain
 * identity, TLS with swapped roles, full identities, then packets. */
#define _DEFAULT_SOURCE /* mkdtemp */
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../harness.h"
#include "cert.h"
#include "conn.h"
#include "link.h"
#include "packet.h"
#include "trust.h"
#include "util.h"

typedef struct {
    kr_cert *cert;
    SSL_CTX *ctx;
    kr_packet *identity;
    kr_trust *trust;
    kr_link_env env;
} side;

static void side_init(side *s, int proto)
{
    s->cert = kr_cert_generate();
    s->ctx = kr_conn_ssl_ctx(kr_cert_x509(s->cert), kr_cert_key(s->cert));
    s->identity = kr_packet_new(KR_PACKET_IDENTITY);
    cJSON *b = s->identity->body;
    cJSON_AddStringToObject(b, "deviceId", kr_cert_device_id(s->cert));
    cJSON_AddStringToObject(b, "deviceName", "Teste");
    cJSON_AddStringToObject(b, "deviceType", "desktop");
    cJSON_AddNumberToObject(b, "protocolVersion", proto);
    cJSON_AddItemToObject(b, "incomingCapabilities", cJSON_CreateArray());
    cJSON_AddItemToObject(b, "outgoingCapabilities", cJSON_CreateArray());
    /* Never written to: nothing gets paired here. */
    s->trust = kr_trust_open("/nonexistent/kiremote-test");
    s->env = (kr_link_env){.ctx = s->ctx, .identity = s->identity, .trust = s->trust,
                           .timeout = 5};
}

static void side_free(side *s)
{
    kr_trust_free(s->trust);
    kr_packet_free(s->identity);
    SSL_CTX_free(s->ctx);
    kr_cert_free(s->cert);
}

/* Pumps both links until both are ready or one closes. Returns the
 * KR_LINK_EV_* bits seen on each. */
static void run(kr_link *a, kr_link *b, int *ev_a, int *ev_b)
{
    *ev_a = *ev_b = 0;
    double start = kr_now();
    while (kr_now() - start < 5) {
        struct pollfd p[2];
        int n = 0;
        bool pa = kr_link_pollfd(a, &p[n]);
        n += pa;
        bool pb = kr_link_pollfd(b, &p[n]);
        n += pb;
        poll(p, (nfds_t)n, 100);
        const char *wa = NULL, *wb = NULL;
        *ev_a |= kr_link_pump(a, pa ? p[0].revents : 0, kr_now(), &wa);
        *ev_b |= kr_link_pump(b, pb ? p[pa].revents : 0, kr_now(), &wb);
        if (*ev_a & KR_LINK_EV_CLOSED)
            fprintf(stderr, "  (a closed: %s)\n", wa);
        if (*ev_b & KR_LINK_EV_CLOSED)
            fprintf(stderr, "  (b closed: %s)\n", wb);
        if ((*ev_a | *ev_b) & KR_LINK_EV_CLOSED)
            return;
        if (kr_link_ready(a) && kr_link_ready(b))
            return;
    }
}

/* a connects to b (a is "outgoing"). */
static void pair_up(side *sa, side *sb, kr_link **a, kr_link **b, const char *target)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        abort();
    *a = kr_link_from_fd(&sa->env, sv[0], target ? target : kr_cert_device_id(sb->cert),
                         KR_PROTOCOL_VERSION, kr_now());
    *b = kr_link_from_fd(&sb->env, sv[1], NULL, 0, kr_now());
}

TEST(handshake_and_packets)
{
    side sa, sb;
    side_init(&sa, 8);
    side_init(&sb, 8);
    kr_link *a, *b;
    pair_up(&sa, &sb, &a, &b, NULL);
    CHECK(a && b);
    CHECK(kr_link_outgoing(a) && !kr_link_outgoing(b));
    int ea, eb;
    run(a, b, &ea, &eb);
    CHECK_INT_EQ(ea, KR_LINK_EV_READY);
    CHECK_INT_EQ(eb, KR_LINK_EV_READY);
    CHECK_STR_EQ(kr_link_device_id(a), kr_cert_device_id(sb.cert));
    CHECK_STR_EQ(kr_link_device_id(b), kr_cert_device_id(sa.cert));
    CHECK(kr_x509_equal(kr_link_cert(a), kr_cert_x509(sb.cert)));
    CHECK(kr_x509_equal(kr_link_cert(b), kr_cert_x509(sa.cert)));
    CHECK_STR_EQ(kr_packet_str(kr_link_identity(b), "deviceType"), "desktop");

    /* A ping one way, a big packet the other. */
    kr_packet *ping = kr_packet_new("kdeconnect.ping");
    CHECK(kr_link_send(a, ping));
    kr_packet_free(ping);
    kr_packet *big = kr_packet_new("kdeconnect.big");
    size_t n = 3u << 20;
    char *blob = malloc(n + 1);
    memset(blob, 'x', n);
    blob[n] = '\0';
    cJSON_AddStringToObject(big->body, "blob", blob);
    CHECK(kr_link_send(b, big));
    kr_packet_free(big);

    kr_packet *got_a = NULL, *got_b = NULL;
    double start = kr_now();
    while ((!got_a || !got_b) && kr_now() - start < 5) {
        struct pollfd p[2];
        kr_link_pollfd(a, &p[0]);
        kr_link_pollfd(b, &p[1]);
        poll(p, 2, 100);
        CHECK(!(kr_link_pump(a, p[0].revents, kr_now(), NULL) & KR_LINK_EV_CLOSED));
        CHECK(!(kr_link_pump(b, p[1].revents, kr_now(), NULL) & KR_LINK_EV_CLOSED));
        if (!got_a)
            got_a = kr_link_next(a);
        if (!got_b)
            got_b = kr_link_next(b);
    }
    CHECK(got_a && got_b);
    CHECK_STR_EQ(got_b->type, "kdeconnect.ping");
    CHECK_STR_EQ(got_a->type, "kdeconnect.big");
    CHECK(strlen(kr_packet_str(got_a, "blob")) == n);
    kr_packet_free(got_a);
    kr_packet_free(got_b);
    free(blob);

    /* Closing one side closes the other. */
    kr_link_free(a);
    int eb2 = 0;
    start = kr_now();
    while (!(eb2 & KR_LINK_EV_CLOSED) && kr_now() - start < 5) {
        struct pollfd p;
        if (kr_link_pollfd(b, &p))
            poll(&p, 1, 100);
        eb2 |= kr_link_pump(b, p.revents, kr_now(), NULL);
    }
    CHECK(eb2 & KR_LINK_EV_CLOSED);
    kr_link_free(b);
    side_free(&sa);
    side_free(&sb);
}

TEST(refuses_a_connection_for_someone_else)
{
    side sa, sb;
    side_init(&sa, 8);
    side_init(&sb, 8);
    kr_link *a, *b;
    pair_up(&sa, &sb, &a, &b, "0123456789abcdef0123456789abcdef");
    int ea, eb;
    run(a, b, &ea, &eb);
    CHECK(eb & KR_LINK_EV_CLOSED);
    CHECK(!kr_link_ready(b));
    kr_link_free(a);
    kr_link_free(b);
    side_free(&sa);
    side_free(&sb);
}

TEST(refuses_old_protocols)
{
    side sa, sb;
    side_init(&sa, 7);
    side_init(&sb, 8);
    kr_link *a, *b;
    pair_up(&sa, &sb, &a, &b, NULL);
    int ea, eb;
    run(a, b, &ea, &eb);
    CHECK(eb & KR_LINK_EV_CLOSED);
    kr_link_free(a);
    kr_link_free(b);
    side_free(&sa);
    side_free(&sb);
}

TEST(refuses_an_id_that_is_not_the_certificates)
{
    side sa, sb;
    side_init(&sa, 8);
    side_init(&sb, 8);
    /* a claims an id its certificate doesn't carry. */
    cJSON_ReplaceItemInObject(sa.identity->body, "deviceId",
                              cJSON_CreateString("0123456789abcdef0123456789abcdef"));
    kr_link *a, *b;
    pair_up(&sa, &sb, &a, &b, NULL);
    int ea, eb;
    run(a, b, &ea, &eb);
    CHECK(eb & KR_LINK_EV_CLOSED);
    CHECK(!kr_link_ready(b));
    kr_link_free(a);
    kr_link_free(b);
    side_free(&sa);
    side_free(&sb);
}

TEST(trusted_device_must_keep_its_certificate)
{
    side sa, sb, impostor;
    side_init(&sa, 8);
    side_init(&sb, 8);
    side_init(&impostor, 8);
    char dir[64];
    snprintf(dir, sizeof dir, "/tmp/kiremote-test-XXXXXX");
    CHECK(mkdtemp(dir));
    /* b paired with a; then someone with another certificate shows up
     * under a's id. */
    kr_trust_free(sb.trust);
    sb.trust = kr_trust_open(dir);
    sb.env.trust = sb.trust;
    CHECK(kr_trust_add(sb.trust, kr_cert_device_id(sa.cert), kr_cert_x509(sa.cert), "a",
                       "phone", 8));

    kr_link *a, *b;
    pair_up(&sa, &sb, &a, &b, NULL);
    int ea, eb;
    run(a, b, &ea, &eb);
    CHECK(kr_link_ready(b)); /* the real one passes */
    kr_link_free(a);
    kr_link_free(b);

    /* The impostor needs a certificate with a's id as CN to get past the
     * CN check; it is the pin that stops it. Make one with its own key. */
    cJSON_ReplaceItemInObject(impostor.identity->body, "deviceId",
                              cJSON_CreateString(kr_cert_device_id(sa.cert)));
    X509 *x = X509_dup(kr_cert_x509(impostor.cert));
    X509_NAME *name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)kr_cert_device_id(sa.cert), -1, -1, 0);
    X509_set_subject_name(x, name);
    X509_set_issuer_name(x, name);
    X509_sign(x, kr_cert_key(impostor.cert), EVP_sha256());
    X509_NAME_free(name);
    SSL_CTX_use_certificate(impostor.ctx, x);
    X509_free(x);

    pair_up(&impostor, &sb, &a, &b, NULL);
    run(a, b, &ea, &eb);
    CHECK(eb & KR_LINK_EV_CLOSED);
    CHECK(!kr_link_ready(b));
    kr_link_free(a);
    kr_link_free(b);

    kr_trust_clear(sb.trust);
    rmdir(dir);
    side_free(&sa);
    side_free(&sb);
    side_free(&impostor);
}

int main(void)
{
    RUN(handshake_and_packets);
    RUN(refuses_a_connection_for_someone_else);
    RUN(refuses_old_protocols);
    RUN(refuses_an_id_that_is_not_the_certificates);
    RUN(trusted_device_must_keep_its_certificate);
    return harness_report();
}
