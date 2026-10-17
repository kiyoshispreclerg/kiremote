/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A scriptable stand-in for the Android app, for tests/net: it speaks the
 * real protocol through the same link code as the daemon.
 *
 *   fakephone -d DIR -t IP:PORT -i DEVICE_ID -n NAME -I caps -O caps
 *
 * It connects to the daemon (as the phone does after seeing its broadcast),
 * prints "ID <its device id>", then:
 *   - every line on stdin is a packet to send, as JSON ({"type":...,
 *     "body":{...}}); "pair" asks to pair, "accept" accepts the daemon's
 *     request;
 *   - "payload FILE JSON" sends the packet with the file as its payload (the
 *     phone serves it on a payload port, as the Android app does);
 *   - every packet received is printed as one line of JSON, prefixed "<< ",
 *     and "READY" / "CLOSED reason" tell the link state.
 * -I and -O are the incoming and outgoing capabilities, comma separated. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cert.h"
#include "conn.h"
#include "link.h"
#include "log.h"
#include "packet.h"
#include "net.h"
#include "transfer.h"
#include "trust.h"
#include "util.h"

static cJSON *caps(const char *list)
{
    cJSON *a = cJSON_CreateArray();
    char *copy = strdup(list ? list : "");
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ","))
        cJSON_AddItemToArray(a, cJSON_CreateString(t));
    free(copy);
    return a;
}

static SSL_CTX *g_ctx;
static kr_transfer *xfers[8];

/* "payload FILE JSON": JSON is the packet, FILE its payload. */
static void send_payload(kr_link *l, char *args)
{
    char *sp = strchr(args, ' ');
    if (!sp) {
        printf("BAD payload\n");
        return;
    }
    *sp = '\0';
    kr_packet *p = kr_packet_parse(sp + 1, strlen(sp + 1));
    int fd = open(args, O_RDONLY | O_CLOEXEC);
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (!p || fd < 0 || size < 0) {
        printf("BAD payload\n");
        kr_packet_free(p);
        if (fd >= 0)
            close(fd);
        return;
    }
    lseek(fd, 0, SEEK_SET);
    int port = 0, slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++)
        if (!xfers[i])
            slot = i;
    kr_transfer *t = slot >= 0 ? kr_transfer_upload(g_ctx, kr_link_cert(l), fd, size, "payload", kr_now(), &port) : NULL;
    if (!t) {
        printf("NOT SENT payload\n");
        kr_packet_free(p);
        return;
    }
    xfers[slot] = t;
    kr_packet_set_payload(p, size, port);
    kr_link_send(l, p);
    kr_packet_free(p);
}

static void send_line(kr_link *l, char *line)
{
    kr_packet *p = NULL;
    if (strncmp(line, "payload ", 8) == 0) {
        send_payload(l, line + 8);
        return;
    }
    if (strcmp(line, "pair") == 0 || strcmp(line, "accept") == 0) {
        p = kr_packet_new(KR_PACKET_PAIR);
        cJSON_AddBoolToObject(p->body, "pair", true);
        if (line[0] == 'p')
            cJSON_AddNumberToObject(p->body, "timestamp", (double)time(NULL));
    } else {
        p = kr_packet_parse(line, strlen(line));
    }
    if (!p) {
        printf("BAD %s\n", line);
        return;
    }
    if (!kr_link_send(l, p) && strcmp(p->type, KR_PACKET_PAIR) != 0)
        printf("NOT SENT %s\n", p->type);
    kr_packet_free(p);
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *target = NULL, *tid = NULL, *name = "Fake Phone";
    const char *in = "", *out = "";
    int c;
    while ((c = getopt(argc, argv, "d:t:i:n:I:O:")) != -1) {
        switch (c) {
        case 'd': dir = optarg; break;
        case 't': target = optarg; break;
        case 'i': tid = optarg; break;
        case 'n': name = optarg; break;
        case 'I': in = optarg; break;
        case 'O': out = optarg; break;
        default: return 2;
        }
    }
    if (!dir || !target || !tid)
        return 2;
    char ip[64];
    int port = 0;
    if (sscanf(target, "%63[^:]:%d", ip, &port) != 2)
        return 2;

    kr_cert *cert = kr_cert_load_or_create(dir, NULL);
    SSL_CTX *ctx = cert ? kr_conn_ssl_ctx(kr_cert_x509(cert), kr_cert_key(cert)) : NULL;
    g_ctx = ctx;
    kr_packet *ident = kr_packet_new(KR_PACKET_IDENTITY);
    if (!ctx || !ident)
        return 1;
    cJSON_AddStringToObject(ident->body, "deviceId", kr_cert_device_id(cert));
    cJSON_AddStringToObject(ident->body, "deviceName", name);
    cJSON_AddStringToObject(ident->body, "deviceType", "phone");
    cJSON_AddNumberToObject(ident->body, "protocolVersion", KR_PROTOCOL_VERSION);
    cJSON_AddItemToObject(ident->body, "incomingCapabilities", caps(in));
    cJSON_AddItemToObject(ident->body, "outgoingCapabilities", caps(out));
    kr_trust *trust = kr_trust_open("/nonexistent/fakephone");
    kr_link_env env = {.ctx = ctx, .identity = ident, .trust = trust, .timeout = 10};

    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, ip, &to.sin_addr);
    kr_link *l = kr_link_connect(&env, &to, tid, KR_PROTOCOL_VERSION, kr_now());
    if (!l) {
        perror("connect");
        return 1;
    }
    printf("ID %s\n", kr_cert_device_id(cert));
    fflush(stdout);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

    char buf[65536];
    size_t blen = 0;
    bool stdin_open = true, ready = false;
    for (;;) {
        struct pollfd p[2 + 8];
        int n = 0, tslot[8], tn = 0;
        bool have_link = kr_link_pollfd(l, &p[n]);
        n += have_link;
        if (stdin_open && ready)
            p[n++] = (struct pollfd){.fd = 0, .events = POLLIN};
        int stdin_idx = stdin_open && ready ? n - 1 : -1;
        for (int i = 0; i < 8; i++)
            if (xfers[i] && kr_transfer_pollfd(xfers[i], &p[n])) {
                tslot[tn++] = i;
                n++;
            }
        poll(p, (nfds_t)n, 100);
        for (int i = 0; i < 8; i++) {
            if (!xfers[i])
                continue;
            short rev = 0;
            for (int k = 0; k < tn; k++)
                if (tslot[k] == i)
                    rev = p[n - tn + k].revents;
            const char *why;
            if (kr_transfer_pump(xfers[i], rev, kr_now(), &why) != KR_TRANSFER_RUNNING) {
                kr_transfer_free(xfers[i]);
                xfers[i] = NULL;
            }
        }
        const char *why = NULL;
        int ev = kr_link_pump(l, have_link ? p[0].revents : 0, kr_now(), &why);
        if ((ev & KR_LINK_EV_READY) && !ready) {
            ready = true;
            printf("READY\n");
        }
        kr_packet *pk;
        while ((pk = kr_link_next(l))) {
            char *s = cJSON_PrintUnformatted(pk->root);
            printf("<< %s\n", s);
            cJSON_free(s);
            kr_packet_free(pk);
        }
        if (ev & KR_LINK_EV_CLOSED) {
            printf("CLOSED %s\n", why ? why : "");
            fflush(stdout);
            return 0;
        }
        if (stdin_idx >= 0 && (p[stdin_idx].revents & (POLLIN | POLLHUP))) {
            ssize_t r = read(0, buf + blen, sizeof buf - 1 - blen);
            if (r == 0)
                stdin_open = false;
            else if (r > 0)
                blen += (size_t)r;
            char *nl;
            buf[blen] = '\0';
            char *start = buf;
            while ((nl = strchr(start, '\n'))) {
                *nl = '\0';
                if (*start)
                    send_line(l, start);
                start = nl + 1;
            }
            blen -= (size_t)(start - buf);
            memmove(buf, start, blen);
        }
        fflush(stdout);
        if (!stdin_open && blen == 0) {
            /* Linger a moment so late replies are printed. */
            static double until;
            if (!until)
                until = kr_now() + 1.0;
            if (kr_now() > until)
                return 0;
        }
    }
}
