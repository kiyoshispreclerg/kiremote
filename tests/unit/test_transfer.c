/* SPDX-License-Identifier: GPL-3.0-or-later */
/* An upload must end by closing the connection: Android reads the payload
 * until EOF and shows an error if the sender never closes. The receiver
 * here does the same, ignoring the size. */
#define _DEFAULT_SOURCE /* mkstemp */
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "cert.h"
#include "conn.h"
#include "net.h"
#include "transfer.h"
#include "util.h"

TEST(upload_closes_when_done)
{
    kr_cert *srv = kr_cert_generate(), *cli = kr_cert_generate();
    SSL_CTX *sctx = kr_conn_ssl_ctx(kr_cert_x509(srv), kr_cert_key(srv));
    SSL_CTX *cctx = kr_conn_ssl_ctx(kr_cert_x509(cli), kr_cert_key(cli));
    CHECK(srv && cli && sctx && cctx);

    size_t n = 2u << 20;
    char path[] = "/tmp/kiremote-xfer-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    unlink(path);
    char *data = malloc(n);
    CHECK(data != NULL);
    for (size_t i = 0; i < n; i++)
        data[i] = (char)(i * 31 + 7);
    CHECK(write(fd, data, n) == (ssize_t)n);
    lseek(fd, 0, SEEK_SET);

    int port = 0;
    kr_transfer *t = kr_transfer_upload(sctx, kr_cert_x509(cli), fd, (int64_t)n, "x", kr_now(), &port);
    CHECK(t && port >= KR_PAYLOAD_FIRST);

    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    kr_conn *c = kr_conn_connect(&to);
    CHECK(c);

    size_t got = 0;
    char *recv_buf = malloc(n);
    bool eof = false, started = false;
    int result = KR_TRANSFER_RUNNING;
    double start = kr_now();
    while (kr_now() - start < 10 && !(eof && result != KR_TRANSFER_RUNNING)) {
        struct pollfd p[2];
        int k = 0, ti = -1, ci = -1;
        if (result == KR_TRANSFER_RUNNING && kr_transfer_pollfd(t, &p[k])) {
            ti = k++;
        }
        if (!eof && kr_conn_pollfd(c, &p[k]))
            ci = k++;
        poll(p, (nfds_t)k, 100);
        if (ti >= 0 || result == KR_TRANSFER_RUNNING) {
            const char *why;
            result = kr_transfer_pump(t, ti >= 0 ? p[ti].revents : 0, kr_now(), &why);
        }
        if (!eof) {
            const char *why;
            int ev = kr_conn_pump(c, ci >= 0 ? p[ci].revents : 0, &why);
            if ((ev & KR_CONN_EV_CONNECTED) && !started) {
                started = true;
                CHECK(kr_conn_start_tls(c, cctx, false));
            }
            size_t len;
            const char *d = kr_conn_data(c, &len);
            if (len) {
                CHECK(got + len <= n);
                memcpy(recv_buf + got, d, len);
                got += len;
                kr_conn_consume(c, len);
            }
            if (ev & KR_CONN_EV_CLOSED)
                eof = true;
        }
    }
    CHECK(eof);                            /* the sender closed */
    CHECK_INT_EQ(result, KR_TRANSFER_DONE);
    CHECK_INT_EQ(got, n);
    CHECK(memcmp(recv_buf, data, n) == 0);

    kr_transfer_free(t);
    kr_conn_free(c);
    free(recv_buf);
    free(data);
    SSL_CTX_free(sctx);
    SSL_CTX_free(cctx);
    kr_cert_free(srv);
    kr_cert_free(cli);
}

int main(void)
{
    RUN(upload_closes_when_done);
    return harness_report();
}
