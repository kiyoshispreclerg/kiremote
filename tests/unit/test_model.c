/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The model the interfaces share: parsing, order, texts, actions, events. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../harness.h"
#include "krmodel.h"
#include "util.h"

#define STATUS \
    "{\"pid\":%d,\"version\":\"0.0.1\",\"id\":\"me\",\"name\":\"Mesa\",\"type\":\"desktop\",\"devices\":[" \
    "{\"id\":\"z-away\",\"name\":\"Tablet velho\",\"type\":\"tablet\",\"reachable\":false,\"paired\":true,\"state\":\"paired\"}," \
    "{\"id\":\"c-near\",\"name\":\"Vizinho\",\"type\":\"phone\",\"reachable\":true,\"paired\":false,\"state\":\"none\"}," \
    "{\"id\":\"b-phone\",\"name\":\"moto g\",\"type\":\"phone\",\"reachable\":true,\"paired\":true,\"state\":\"paired\"," \
    "\"address\":\"192.168.0.9\",\"plugins\":[\"ping\",\"share\",\"findmyphone\",\"sftp\",\"battery\"]," \
    "\"battery\":{\"charge\":80,\"charging\":true},\"signal\":{\"type\":\"LTE\",\"strength\":3}}," \
    "{\"id\":\"a-ask\",\"name\":\"Zé\",\"type\":\"phone\",\"reachable\":true,\"paired\":false,\"state\":\"requested_by_peer\",\"key\":\"1A2B3C4D\"}," \
    "{\"id\":\"d-phone\",\"name\":\"Antigo\",\"type\":\"phone\",\"reachable\":true,\"paired\":true,\"state\":\"paired\"," \
    "\"plugins\":[\"ping\"],\"battery\":{\"charge\":15,\"charging\":false},\"signal\":{\"type\":\"\",\"strength\":-1}}" \
    "]}"

static cJSON *status(void)
{
    char buf[4096];
    snprintf(buf, sizeof buf, STATUS, (int)getpid());
    return cJSON_Parse(buf);
}

static kr_model *loaded(void)
{
    kr_model *m = kr_model_new();
    cJSON *st = status();
    kr_model_apply_status(m, st);
    cJSON_Delete(st);
    return m;
}

TEST(a_new_model_is_down_and_empty)
{
    kr_model *m = kr_model_new();
    CHECK(!kr_model_daemon_up(m));
    CHECK_INT_EQ(kr_model_count(m), 0);
    CHECK(kr_model_at(m, 0) == NULL);
    CHECK(kr_model_find(m, "x") == NULL);
    char tip[200];
    kr_model_tooltip(m, tip, sizeof tip);
    CHECK_STR_HAS(tip, "not running");
    kr_model_free(m);
}

TEST(status_replaces_everything)
{
    kr_model *m = loaded();
    CHECK(kr_model_daemon_up(m));
    CHECK_STR_EQ(kr_model_name(m), "Mesa");
    CHECK_STR_EQ(kr_model_version(m), "0.0.1");
    CHECK_INT_EQ(kr_model_count(m), 5);

    cJSON *empty = cJSON_Parse("{\"name\":\"Mesa\",\"devices\":[]}");
    CHECK(kr_model_apply_status(m, empty));
    CHECK_INT_EQ(kr_model_count(m), 0);
    CHECK(kr_model_daemon_up(m));
    cJSON_Delete(empty);

    cJSON *junk = cJSON_Parse("{\"name\":\"x\"}");
    CHECK(!kr_model_apply_status(m, junk));
    cJSON_Delete(junk);
    CHECK(!kr_model_apply_status(m, NULL));
    kr_model_free(m);
}

/* Requests first, then connected, then away, then the others; by name
 * (ignoring case) inside each. */
TEST(order_of_the_list)
{
    kr_model *m = loaded();
    /* Antigo before moto g: case is ignored. */
    const char *want[] = {"a-ask", "d-phone", "b-phone", "z-away", "c-near"};
    for (int i = 0; i < 5; i++)
        CHECK_STR_EQ(kr_model_at(m, i)->id, want[i]);
    CHECK_INT_EQ(kr_model_connected(m), 2);
    CHECK(kr_model_pair_pending(m));
    kr_model_free(m);
}

TEST(parsing_a_device)
{
    kr_model *m = loaded();
    const kr_dev *d = kr_model_find(m, "b-phone");
    CHECK(d);
    CHECK_STR_EQ(d->name, "moto g");
    CHECK_STR_EQ(d->address, "192.168.0.9");
    CHECK(d->paired && d->reachable);
    CHECK_INT_EQ(d->battery, 80);
    CHECK(d->charging);
    CHECK(d->has_signal);
    CHECK_INT_EQ(d->signal, 3);
    CHECK_STR_EQ(d->net_type, "LTE");
    CHECK_INT_EQ(d->n_plugins, 5);

    const kr_dev *z = kr_model_find(m, "z-away");
    CHECK_INT_EQ(z->battery, -1);
    CHECK(!z->has_signal);
    CHECK_INT_EQ(z->n_plugins, 0);
    CHECK_STR_EQ(kr_model_find(m, "a-ask")->key, "1A2B3C4D");
    kr_model_free(m);
}

TEST(bad_values_do_not_break_it)
{
    kr_model *m = kr_model_new();
    cJSON *st = cJSON_Parse("{\"devices\":[{\"id\":\"x\",\"name\":7,\"battery\":{\"charge\":900},"
                            "\"signal\":{\"strength\":99},\"plugins\":[1,\"ping\",null]},{\"name\":\"no id\"}]}");
    CHECK(kr_model_apply_status(m, st));
    cJSON_Delete(st);
    CHECK_INT_EQ(kr_model_count(m), 1); /* the one without an id is dropped */
    const kr_dev *d = kr_model_at(m, 0);
    CHECK_STR_EQ(d->name, "");
    CHECK_INT_EQ(d->battery, -1);
    CHECK_INT_EQ(d->signal, -1);
    CHECK_INT_EQ(d->n_plugins, 1);
    kr_model_free(m);
}

TEST(texts)
{
    kr_model *m = loaded();
    char buf[64];
    const kr_dev *d = kr_model_find(m, "b-phone");
    CHECK_STR_EQ(kr_dev_state_text(d), "Connected");
    CHECK_STR_EQ(kr_dev_battery_text(d, buf, sizeof buf), "80% (charging)");
    CHECK_STR_EQ(kr_dev_signal_text(d, buf, sizeof buf), "LTE 3/4");
    CHECK_STR_EQ(kr_dev_icon(d), "phone");

    d = kr_model_find(m, "d-phone");
    CHECK_STR_EQ(kr_dev_battery_text(d, buf, sizeof buf), "15%");
    CHECK_STR_EQ(kr_dev_signal_text(d, buf, sizeof buf), "No signal");

    d = kr_model_find(m, "z-away");
    CHECK_STR_EQ(kr_dev_state_text(d), "Not reachable");
    CHECK_STR_EQ(kr_dev_battery_text(d, buf, sizeof buf), "");
    CHECK_STR_EQ(kr_dev_signal_text(d, buf, sizeof buf), "");
    CHECK_STR_EQ(kr_dev_icon(d), "tablet");

    CHECK_STR_EQ(kr_dev_state_text(kr_model_find(m, "a-ask")), "Wants to pair");
    CHECK_STR_EQ(kr_dev_state_text(kr_model_find(m, "c-near")), "Available");
    kr_model_free(m);
}

#define HAS(mask, a) (((mask) & (1u << (a))) != 0)

/* The menu is the same in every interface because it comes from here. */
TEST(actions_follow_the_state_and_the_plugins)
{
    kr_model *m = loaded();
    unsigned a = kr_dev_actions(kr_model_find(m, "b-phone"));
    CHECK(HAS(a, KR_ACT_PING) && HAS(a, KR_ACT_FIND) && HAS(a, KR_ACT_SEND_FILES));
    CHECK(HAS(a, KR_ACT_SEND_TEXT) && HAS(a, KR_ACT_BROWSE) && HAS(a, KR_ACT_UNPAIR));
    CHECK(!HAS(a, KR_ACT_PAIR) && !HAS(a, KR_ACT_REFRESH));

    /* Only ping: no files, no ringing. */
    a = kr_dev_actions(kr_model_find(m, "d-phone"));
    CHECK(HAS(a, KR_ACT_PING) && HAS(a, KR_ACT_UNPAIR));
    CHECK(!HAS(a, KR_ACT_FIND) && !HAS(a, KR_ACT_SEND_FILES) && !HAS(a, KR_ACT_BROWSE));

    /* Away: it can only be forgotten. */
    CHECK_INT_EQ(kr_dev_actions(kr_model_find(m, "z-away")), 1u << KR_ACT_UNPAIR);
    CHECK_INT_EQ(kr_dev_actions(kr_model_find(m, "c-near")), 1u << KR_ACT_PAIR);
    CHECK_INT_EQ(kr_dev_actions(kr_model_find(m, "a-ask")), (1u << KR_ACT_ACCEPT) | (1u << KR_ACT_REFUSE));

    /* Mirroring: only when the host found kiremote-mirror, and only for a
     * paired phone that is there. */
    CHECK(!HAS(kr_dev_actions(kr_model_find(m, "b-phone")), KR_ACT_MIRROR));
    CHECK(!HAS(kr_dev_actions(kr_model_find(m, "b-phone")), KR_ACT_CAMERA));
    kr_actions_set_mirror(true);
    CHECK(HAS(kr_dev_actions(kr_model_find(m, "b-phone")), KR_ACT_MIRROR));
    CHECK(HAS(kr_dev_actions(kr_model_find(m, "b-phone")), KR_ACT_CAMERA));
    CHECK_INT_EQ(kr_dev_actions(kr_model_find(m, "z-away")), 1u << KR_ACT_UNPAIR);
    CHECK_INT_EQ(kr_dev_actions(kr_model_find(m, "c-near")), 1u << KR_ACT_PAIR);
    kr_actions_set_mirror(false);

    cJSON *ev = cJSON_Parse("{\"event\":\"device\",\"device\":{\"id\":\"c-near\",\"name\":\"Vizinho\",\"state\":\"requested\"}}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, NULL), KR_EV_DEVICES);
    cJSON_Delete(ev);
    CHECK_INT_EQ(kr_dev_actions(kr_model_find(m, "c-near")), 1u << KR_ACT_CANCEL);
    kr_model_free(m);
}

TEST(every_action_has_a_label_and_a_request)
{
    for (int a = 0; a < KR_ACT_COUNT; a++) {
        CHECK_MSG(kr_action_label((kr_action)a)[0], "no label for %d", a);
        cJSON *r = kr_action_request((kr_action)a, "dev", "text");
        if (a == KR_ACT_SEND_FILES) {
            CHECK(!r && kr_action_needs_input((kr_action)a));
            continue;
        }
        if (a == KR_ACT_MIRROR || a == KR_ACT_CAMERA) { /* a program the host runs, not a request */
            CHECK(!r);
            continue;
        }
        CHECK_MSG(r, "no request for %d", a);
        const cJSON *t = cJSON_GetObjectItem(r, "type");
        CHECK(cJSON_IsString(t));
        if (a != KR_ACT_REFRESH)
            CHECK_STR_EQ(cJSON_GetObjectItem(r, "device")->valuestring, "dev");
        cJSON_Delete(r);
    }
}

TEST(requests)
{
    cJSON *r = kr_action_request(KR_ACT_PING, "id1", NULL);
    CHECK_STR_EQ(cJSON_GetObjectItem(r, "type")->valuestring, "ping");
    cJSON_Delete(r);
    r = kr_action_request(KR_ACT_FIND, "id1", NULL);
    CHECK_STR_EQ(cJSON_GetObjectItem(r, "type")->valuestring, "find-phone");
    cJSON_Delete(r);
    r = kr_action_request(KR_ACT_ACCEPT, "id1", NULL);
    CHECK_STR_EQ(cJSON_GetObjectItem(r, "type")->valuestring, "pair");
    cJSON_Delete(r);
    r = kr_action_request(KR_ACT_REFUSE, "id1", NULL);
    CHECK_STR_EQ(cJSON_GetObjectItem(r, "type")->valuestring, "unpair");
    cJSON_Delete(r);
    r = kr_action_request(KR_ACT_SEND_TEXT, "id1", "olá");
    CHECK_STR_EQ(cJSON_GetObjectItem(r, "type")->valuestring, "share-text");
    CHECK_STR_EQ(cJSON_GetObjectItem(r, "text")->valuestring, "olá");
    cJSON_Delete(r);
    CHECK(kr_action_request(KR_ACT_SEND_TEXT, "id1", "") == NULL);
    CHECK(kr_action_request(KR_ACT_SEND_TEXT, "id1", NULL) == NULL);

    const char *paths[] = {"/a/b", "/c d"};
    r = kr_action_files("id1", paths, 2);
    CHECK_STR_EQ(cJSON_GetObjectItem(r, "type")->valuestring, "share");
    CHECK_INT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(r, "paths")), 2);
    CHECK_STR_EQ(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "paths"), 1)->valuestring, "/c d");
    cJSON_Delete(r);
    CHECK(kr_action_files("id1", paths, 0) == NULL);

    CHECK(kr_action_destructive(KR_ACT_UNPAIR) && kr_action_destructive(KR_ACT_REFUSE));
    CHECK(!kr_action_destructive(KR_ACT_PING));
}

TEST(events)
{
    kr_model *m = loaded();
    const char *id = NULL;

    cJSON *ev = cJSON_Parse("{\"event\":\"device\",\"device\":{\"id\":\"b-phone\",\"name\":\"moto g\","
                            "\"paired\":true,\"reachable\":true,\"state\":\"paired\",\"battery\":{\"charge\":79,\"charging\":false}}}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_DEVICES);
    CHECK_STR_EQ(id, "b-phone");
    cJSON_Delete(ev);
    CHECK_INT_EQ(kr_model_count(m), 5);
    CHECK_INT_EQ(kr_model_find(m, "b-phone")->battery, 79);
    CHECK_INT_EQ(kr_model_find(m, "b-phone")->n_plugins, 0); /* replaced, not merged */

    ev = cJSON_Parse("{\"event\":\"device\",\"device\":{\"id\":\"new\",\"name\":\"Aaa\",\"reachable\":true,\"state\":\"none\"}}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_DEVICES);
    cJSON_Delete(ev);
    CHECK_INT_EQ(kr_model_count(m), 6);
    CHECK_STR_EQ(kr_model_at(m, 5)->id, "c-near"); /* "Aaa" sorts before "Vizinho" */
    CHECK_STR_EQ(kr_model_at(m, 4)->id, "new");

    ev = cJSON_Parse("{\"event\":\"pair-request\",\"device\":{\"id\":\"p\",\"name\":\"Pedinte\",\"state\":\"requested_by_peer\",\"key\":\"00112233\"}}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_PAIR_REQUEST);
    CHECK_STR_EQ(id, "p");
    cJSON_Delete(ev);
    CHECK_STR_EQ(kr_model_find(m, "p")->key, "00112233");

    ev = cJSON_Parse("{\"event\":\"removed\",\"id\":\"new\"}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_DEVICES);
    CHECK_STR_EQ(id, "new");
    cJSON_Delete(ev);
    CHECK(kr_model_find(m, "new") == NULL);
    CHECK_INT_EQ(kr_model_count(m), 6);

    ev = cJSON_Parse("{\"event\":\"removed\",\"id\":\"never-was\"}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_NONE);
    CHECK(id == NULL);
    cJSON_Delete(ev);

    ev = cJSON_Parse("{\"event\":\"config\",\"key\":\"share.dir\",\"value\":\"/x\"}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_CONFIG);
    CHECK_STR_EQ(id, "share.dir");
    cJSON_Delete(ev);

    /* Not events, or ones we don't know. */
    ev = cJSON_Parse("{\"ok\":true}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_NONE);
    cJSON_Delete(ev);
    ev = cJSON_Parse("{\"event\":\"from-the-future\"}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_NONE);
    cJSON_Delete(ev);
    ev = cJSON_Parse("{\"event\":\"device\"}");
    CHECK_INT_EQ(kr_model_apply_event(m, ev, &id), KR_EV_NONE);
    cJSON_Delete(ev);
    CHECK_INT_EQ(kr_model_apply_event(m, NULL, &id), KR_EV_NONE);
    kr_model_free(m);
}

TEST(reset_forgets_the_devices)
{
    kr_model *m = loaded();
    CHECK(kr_model_reset(m));
    CHECK(!kr_model_daemon_up(m));
    CHECK_INT_EQ(kr_model_count(m), 0);
    CHECK_STR_EQ(kr_model_name(m), "");
    CHECK(!kr_model_reset(m)); /* nothing left to change */
    kr_model_free(m);
}

TEST(tooltip)
{
    kr_model *m = loaded();
    char tip[300];
    kr_model_tooltip(m, tip, sizeof tip);
    CHECK_STR_EQ(tip, "Antigo - 15%\nmoto g - 80% (charging)");

    char tiny[8];
    kr_model_tooltip(m, tiny, sizeof tiny); /* cut, not overrun */
    CHECK(strlen(tiny) < sizeof tiny);

    cJSON *st = cJSON_Parse("{\"devices\":[{\"id\":\"q\",\"name\":\"Q\",\"state\":\"requested_by_peer\"}]}");
    kr_model_apply_status(m, st);
    cJSON_Delete(st);
    kr_model_tooltip(m, tip, sizeof tip);
    CHECK_STR_EQ(tip, "A device wants to pair");

    st = cJSON_Parse("{\"devices\":[]}");
    kr_model_apply_status(m, st);
    cJSON_Delete(st);
    kr_model_tooltip(m, tip, sizeof tip);
    CHECK_STR_EQ(tip, "No device connected");
    kr_model_free(m);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

TEST(status_file)
{
    char dir[] = "/tmp/krmodel.XXXXXX";
    CHECK(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof path, "%s/kiremote-status.json", dir);
    kr_model *m = kr_model_new();

    /* No file: no daemon. */
    CHECK(!kr_model_load_file(m, path));
    CHECK(!kr_model_daemon_up(m));

    char buf[4096];
    snprintf(buf, sizeof buf, STATUS, (int)getpid());
    write_file(path, buf);
    CHECK(kr_model_load_file(m, path));
    CHECK(kr_model_daemon_up(m));
    CHECK_INT_EQ(kr_model_count(m), 5);
    CHECK(!kr_model_load_file(m, path)); /* same text: nothing to redraw */

    write_file(path, "{\"pid\":1,\"devices\":[]}");
    /* pid 1 is alive (init), so the new content counts. */
    CHECK(kr_model_load_file(m, path));
    CHECK_INT_EQ(kr_model_count(m), 0);

    /* A daemon that crashed leaves a file with a pid nobody has. */
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    waitpid(child, NULL, 0);
    snprintf(buf, sizeof buf, STATUS, (int)child);
    write_file(path, buf);
    CHECK(kr_model_load_file(m, path) || !kr_model_daemon_up(m));
    CHECK(!kr_model_daemon_up(m));
    CHECK_INT_EQ(kr_model_count(m), 0);

    /* Garbage. */
    write_file(path, "not json");
    kr_model_load_file(m, path);
    CHECK(!kr_model_daemon_up(m));

    unlink(path);
    rmdir(dir);
    kr_model_free(m);
}

TEST(status_path_is_beside_the_socket)
{
    char buf[200];
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    CHECK(kr_model_status_path(buf, sizeof buf));
    CHECK_STR_EQ(buf, "/run/user/1000/kiremote-status.json");
}

int main(void)
{
    RUN(a_new_model_is_down_and_empty);
    RUN(status_replaces_everything);
    RUN(order_of_the_list);
    RUN(parsing_a_device);
    RUN(bad_values_do_not_break_it);
    RUN(texts);
    RUN(actions_follow_the_state_and_the_plugins);
    RUN(every_action_has_a_label_and_a_request);
    RUN(requests);
    RUN(events);
    RUN(reset_forgets_the_devices);
    RUN(tooltip);
    RUN(status_file);
    RUN(status_path_is_beside_the_socket);
    return harness_report();
}
