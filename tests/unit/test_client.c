/* SPDX-License-Identifier: GPL-3.0-or-later */
/* krclient against the real control socket code with a scripted daemon:
 * snapshot, events, requests and replies, losing and finding the daemon. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "control.h"
#include "krclient.h"
#include "util.h"

typedef struct {
    kr_control *srv;
    kr_client *cli;
    char dir[64], path[96];
    /* what the client was told */
    int daemon_changes, device_changes, pair_requests, config_changes;
    char last_id[96];
    /* replies */
    int replies;
    bool last_was_null;
    char last_reply[256];
    char order[64];
} rig;

static void on_request(void *ud, const char *type, const cJSON *req, cJSON *reply)
{
    (void)ud;
    (void)req;
    if (strcmp(type, "status") == 0) {
        cJSON_AddStringToObject(reply, "name", "Mesa");
        cJSON_AddStringToObject(reply, "version", "9.9");
        cJSON *a = cJSON_AddArrayToObject(reply, "devices");
        cJSON *d = cJSON_Parse("{\"id\":\"p1\",\"name\":\"Phone\",\"paired\":true,\"reachable\":true,\"state\":\"paired\","
                               "\"plugins\":[\"ping\"]}");
        cJSON_AddItemToArray(a, d);
    } else if (strcmp(type, "ping") == 0 || strcmp(type, "pong") == 0) {
        cJSON_AddStringToObject(reply, "which", type);
    } else {
        kr_control_fail(reply, "no such thing");
    }
}

static void on_change(void *ud, kr_change what, const char *id)
{
    rig *r = ud;
    switch (what) {
    case KR_CH_DAEMON:
        r->daemon_changes++;
        break;
    case KR_CH_DEVICES:
        r->device_changes++;
        break;
    case KR_CH_PAIR_REQUEST:
        r->pair_requests++;
        break;
    case KR_CH_CONFIG:
        r->config_changes++;
        break;
    }
    snprintf(r->last_id, sizeof r->last_id, "%s", id ? id : "");
}

static void on_reply(void *ud, const cJSON *reply)
{
    rig *r = ud;
    r->replies++;
    r->last_was_null = reply == NULL;
    char *t = reply ? cJSON_PrintUnformatted(reply) : NULL;
    snprintf(r->last_reply, sizeof r->last_reply, "%s", t ? t : "");
    free(t);
}

static void on_reply_ordered(void *ud, const cJSON *reply)
{
    rig *r = ud;
    const cJSON *w = cJSON_GetObjectItem(reply, "which");
    size_t n = strlen(r->order);
    snprintf(r->order + n, sizeof r->order - n, "%s;", cJSON_IsString(w) ? w->valuestring : "?");
}

static bool serve_open(rig *r)
{
    r->srv = kr_control_open(r->path, on_request, NULL);
    return r->srv != NULL;
}

/* Runs the daemon side and the client side together for a while. */
static void spin(rig *r, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        struct pollfd f[40];
        int n = r->srv ? kr_control_pollfds(r->srv, f, 39) : 0;
        int srv_n = n;
        if (r->cli && kr_client_fd(r->cli) >= 0)
            f[n++] = (struct pollfd){.fd = kr_client_fd(r->cli), .events = kr_client_events(r->cli)};
        poll(f, (nfds_t)n, 10);
        if (r->srv)
            kr_control_dispatch(r->srv, f, srv_n);
        if (r->cli && n > srv_n)
            kr_client_dispatch(r->cli, f[srv_n].revents);
    }
}

static bool rig_start(rig *r, bool server_first)
{
    memset(r, 0, sizeof *r);
    snprintf(r->dir, sizeof r->dir, "/tmp/krcli.XXXXXX");
    if (!mkdtemp(r->dir))
        return false;
    snprintf(r->path, sizeof r->path, "%s/s.sock", r->dir);
    if (server_first && !serve_open(r))
        return false;
    static const kr_client_ops ops = {on_change};
    r->cli = kr_client_new(r->path, &ops, r);
    return r->cli != NULL;
}

static void rig_stop(rig *r)
{
    kr_client_free(r->cli);
    kr_control_close(r->srv);
    unlink(r->path);
    rmdir(r->dir);
}

static void broadcast(rig *r, const char *line)
{
    kr_control_broadcast(r->srv, line);
    spin(r, 5);
}

TEST(gets_the_snapshot_on_connecting)
{
    rig r;
    CHECK(rig_start(&r, true));
    CHECK(!kr_model_daemon_up(kr_client_model(r.cli)));
    spin(&r, 10);
    const kr_model *m = kr_client_model(r.cli);
    CHECK(kr_model_daemon_up(m));
    CHECK_STR_EQ(kr_model_name(m), "Mesa");
    CHECK_INT_EQ(kr_model_count(m), 1);
    CHECK_STR_EQ(kr_model_at(m, 0)->name, "Phone");
    CHECK_INT_EQ(r.daemon_changes, 1);
    CHECK(r.device_changes >= 1);
    CHECK(kr_control_has_subscribers(r.srv));
    CHECK_INT_EQ(kr_client_deadline(r.cli) == 0, 1);
    rig_stop(&r);
}

TEST(events_update_the_model)
{
    rig r;
    CHECK(rig_start(&r, true));
    spin(&r, 10);
    const kr_model *m = kr_client_model(r.cli);
    int before = r.device_changes;

    broadcast(&r, "{\"event\":\"device\",\"device\":{\"id\":\"p1\",\"name\":\"Phone\",\"paired\":true,\"reachable\":true,"
                  "\"state\":\"paired\",\"battery\":{\"charge\":55,\"charging\":false}}}");
    CHECK_INT_EQ(r.device_changes, before + 1);
    CHECK_STR_EQ(r.last_id, "p1");
    CHECK_INT_EQ(kr_model_find(m, "p1")->battery, 55);

    broadcast(&r, "{\"event\":\"pair-request\",\"device\":{\"id\":\"p2\",\"name\":\"Novo\",\"state\":\"requested_by_peer\",\"key\":\"AABBCCDD\"}}");
    CHECK_INT_EQ(r.pair_requests, 1);
    CHECK_STR_EQ(r.last_id, "p2");
    CHECK(kr_model_pair_pending(m));

    broadcast(&r, "{\"event\":\"config\",\"key\":\"share.dir\",\"value\":\"/x\"}");
    CHECK_INT_EQ(r.config_changes, 1);
    CHECK_STR_EQ(r.last_id, "share.dir");

    broadcast(&r, "{\"event\":\"removed\",\"id\":\"p2\"}");
    CHECK(kr_model_find(m, "p2") == NULL);
    CHECK(!kr_model_pair_pending(m));

    /* Garbage on the line is ignored. */
    broadcast(&r, "not json at all");
    broadcast(&r, "{\"event\":\"from-the-future\"}");
    CHECK(kr_model_daemon_up(m));
    rig_stop(&r);
}

TEST(requests_get_their_replies_in_order)
{
    rig r;
    CHECK(rig_start(&r, true));
    spin(&r, 10);
    CHECK(kr_client_send(r.cli, cJSON_Parse("{\"type\":\"ping\"}"), on_reply_ordered, &r));
    CHECK(kr_client_send(r.cli, cJSON_Parse("{\"type\":\"pong\"}"), on_reply_ordered, &r));
    CHECK(kr_client_send(r.cli, cJSON_Parse("{\"type\":\"ping\"}"), on_reply_ordered, &r));
    spin(&r, 10);
    CHECK_STR_EQ(r.order, "ping;pong;ping;");

    /* An event between a request and its reply doesn't take its place. */
    CHECK(kr_client_send(r.cli, cJSON_Parse("{\"type\":\"nope\"}"), on_reply, &r));
    kr_control_broadcast(r.srv, "{\"event\":\"config\",\"key\":\"a.b\"}");
    spin(&r, 10);
    CHECK_INT_EQ(r.replies, 1);
    CHECK(!r.last_was_null);
    CHECK_STR_HAS(r.last_reply, "\"ok\":false");
    cJSON *bad = cJSON_Parse(r.last_reply);
    CHECK_STR_EQ(kr_reply_error(bad), "no such thing");
    cJSON_Delete(bad);
    cJSON *good = cJSON_Parse("{\"ok\":true}");
    CHECK(kr_reply_error(good) == NULL);
    cJSON_Delete(good);
    CHECK(kr_reply_error(NULL) != NULL);
    rig_stop(&r);
}

TEST(sending_without_a_daemon_fails_at_once)
{
    rig r;
    CHECK(rig_start(&r, false));
    CHECK_INT_EQ(kr_client_fd(r.cli), -1);
    CHECK(!kr_client_send(r.cli, cJSON_Parse("{\"type\":\"ping\"}"), on_reply, &r));
    CHECK_INT_EQ(r.replies, 1);
    CHECK(r.last_was_null);
    CHECK(!kr_client_send(r.cli, NULL, on_reply, &r));
    CHECK_INT_EQ(r.replies, 2);
    rig_stop(&r);
}

TEST(finds_a_daemon_that_starts_later)
{
    rig r;
    CHECK(rig_start(&r, false));
    CHECK(kr_client_deadline(r.cli) > 0);
    CHECK(!kr_model_daemon_up(kr_client_model(r.cli)));

    kr_client_tick(r.cli, kr_now()); /* too early: nothing */
    CHECK_INT_EQ(kr_client_fd(r.cli), -1);

    CHECK(serve_open(&r));
    kr_client_tick(r.cli, kr_client_deadline(r.cli) + 0.01);
    CHECK(kr_client_fd(r.cli) >= 0);
    spin(&r, 10);
    CHECK(kr_model_daemon_up(kr_client_model(r.cli)));
    CHECK_INT_EQ(r.daemon_changes, 1);
    rig_stop(&r);
}

TEST(survives_the_daemon_restarting)
{
    rig r;
    CHECK(rig_start(&r, true));
    spin(&r, 10);
    CHECK(kr_model_daemon_up(kr_client_model(r.cli)));

    /* A request is waiting when the daemon dies. */
    CHECK(kr_client_send(r.cli, cJSON_Parse("{\"type\":\"ping\"}"), on_reply, &r));
    kr_control_close(r.srv);
    r.srv = NULL;
    spin(&r, 5);
    CHECK_INT_EQ(r.replies, 1);
    CHECK(r.last_was_null);
    CHECK(!kr_model_daemon_up(kr_client_model(r.cli)));
    CHECK_INT_EQ(kr_model_count(kr_client_model(r.cli)), 0);
    CHECK_INT_EQ(r.daemon_changes, 2); /* up, then down */
    CHECK_INT_EQ(kr_client_fd(r.cli), -1);
    CHECK(kr_client_deadline(r.cli) > 0);

    /* It comes back. */
    CHECK(serve_open(&r));
    kr_client_tick(r.cli, kr_client_deadline(r.cli) + 0.01);
    spin(&r, 10);
    CHECK(kr_model_daemon_up(kr_client_model(r.cli)));
    CHECK_INT_EQ(kr_model_count(kr_client_model(r.cli)), 1);
    CHECK_INT_EQ(r.daemon_changes, 3);
    CHECK(kr_client_send(r.cli, cJSON_Parse("{\"type\":\"pong\"}"), on_reply, &r));
    spin(&r, 10);
    CHECK_INT_EQ(r.replies, 2);
    CHECK(!r.last_was_null);
    rig_stop(&r);
}

TEST(retry_waits_longer_each_time_up_to_a_limit)
{
    rig r;
    CHECK(rig_start(&r, false));
    double last_gap = 0;
    for (int i = 0; i < 8; i++) {
        double d = kr_client_deadline(r.cli);
        kr_client_tick(r.cli, d + 0.001);
        double gap = kr_client_deadline(r.cli) - kr_now();
        CHECK(gap <= 5.01);
        last_gap = gap;
    }
    CHECK(last_gap > 4.0);
    rig_stop(&r);
}

TEST(freeing_with_requests_waiting_answers_them)
{
    rig r;
    CHECK(rig_start(&r, true));
    spin(&r, 10);
    CHECK(kr_client_send(r.cli, cJSON_Parse("{\"type\":\"ping\"}"), on_reply, &r));
    kr_client_free(r.cli);
    r.cli = NULL;
    CHECK_INT_EQ(r.replies, 1);
    CHECK(r.last_was_null);
    rig_stop(&r);
}

int main(void)
{
    RUN(gets_the_snapshot_on_connecting);
    RUN(events_update_the_model);
    RUN(requests_get_their_replies_in_order);
    RUN(sending_without_a_daemon_fails_at_once);
    RUN(finds_a_daemon_that_starts_later);
    RUN(survives_the_daemon_restarting);
    RUN(retry_waits_longer_each_time_up_to_a_limit);
    RUN(freeing_with_requests_waiting_answers_them);
    return harness_report();
}
