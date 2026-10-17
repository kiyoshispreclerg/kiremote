/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Packets: parsing, serializing, identity checks. */
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "packet.h"

static kr_packet *parse(const char *s)
{
    return kr_packet_parse(s, strlen(s));
}

TEST(parses_a_packet)
{
    kr_packet *p = parse("{\"id\":1,\"type\":\"kdeconnect.ping\",\"body\":{\"message\":\"oi\"}}\n");
    CHECK(p);
    CHECK_STR_EQ(p->type, "kdeconnect.ping");
    CHECK_STR_EQ(kr_packet_str(p, "message"), "oi");
    CHECK_INT_EQ(p->payload_size, -1);
    CHECK_INT_EQ(p->payload_port, 0);
    kr_packet_free(p);
}

TEST(body_is_optional_and_type_is_not)
{
    kr_packet *p = parse("{\"type\":\"kdeconnect.ping\"}");
    CHECK(p && cJSON_IsObject(p->body));
    kr_packet_free(p);
    p = parse("{\"type\":\"x\",\"body\":[1,2]}");
    CHECK(p && cJSON_IsObject(p->body) && !p->body->child);
    kr_packet_free(p);

    CHECK(!parse("{\"body\":{}}"));
    CHECK(!parse("{\"type\":\"\"}"));
    CHECK(!parse("{\"type\":3}"));
    CHECK(!parse("[]"));
    CHECK(!parse("{\"type\":\"x\""));
    CHECK(!parse(""));
}

TEST(payload_fields)
{
    kr_packet *p = parse("{\"type\":\"kdeconnect.share.request\",\"body\":{\"filename\":\"a\"},"
                         "\"payloadSize\":12345678901,\"payloadTransferInfo\":{\"port\":1739}}");
    CHECK(p);
    CHECK_INT_EQ(p->payload_size, 12345678901LL);
    CHECK_INT_EQ(p->payload_port, 1739);
    kr_packet_free(p);

    p = parse("{\"type\":\"x\",\"payloadSize\":5,\"payloadTransferInfo\":{\"port\":70000}}");
    CHECK(p && p->payload_port == 0);
    kr_packet_free(p);
}

TEST(serializes_one_line)
{
    kr_packet *p = kr_packet_new("kdeconnect.ping");
    CHECK(p);
    cJSON_AddStringToObject(p->body, "message", "a/b");
    size_t len;
    char *s = kr_packet_serialize(p, &len);
    CHECK(s);
    CHECK_INT_EQ(len, strlen(s));
    CHECK(s[len - 1] == '\n');
    CHECK(!memchr(s, '\n', len - 1));
    CHECK(strncmp(s, "{\"id\":", 6) == 0);
    CHECK_STR_HAS(s, "\"type\":\"kdeconnect.ping\",\"body\":{\"message\":\"a/b\"}}");
    CHECK(!strstr(s, "payload"));

    /* Round trip, with a payload. */
    kr_packet_set_payload(p, 42, 1740);
    free(s);
    s = kr_packet_serialize(p, &len);
    kr_packet *q = kr_packet_parse(s, len);
    CHECK(q);
    CHECK_STR_EQ(q->type, "kdeconnect.ping");
    CHECK_INT_EQ(q->payload_size, 42);
    CHECK_INT_EQ(q->payload_port, 1740);
    int64_t id = 0;
    CHECK(cJSON_IsNumber(cJSON_GetObjectItem(q->root, "id")));
    id = (int64_t)cJSON_GetObjectItem(q->root, "id")->valuedouble;
    CHECK(id > 1600000000000LL); /* milliseconds */
    kr_packet_free(q);
    free(s);
    kr_packet_free(p);
}

TEST(device_ids)
{
    CHECK(kr_device_id_valid("0123456789abcdef0123456789abcdef"));
    CHECK(kr_device_id_valid("_123456789abcdef0123456789abcde-"));
    CHECK(kr_device_id_valid("0123456789abcdef0123456789abcdef012345")); /* 38 */
    CHECK(!kr_device_id_valid("0123456789abcdef0123456789abcdef0123456")); /* 39 */
    CHECK(!kr_device_id_valid("0123456789abcdef0123456789abcde")); /* 31 */
    CHECK(!kr_device_id_valid("0123456789abcdef0123456789abcde/"));
    CHECK(!kr_device_id_valid("0123456789abcdef 0123456789abcdef"));
    CHECK(!kr_device_id_valid(""));
    CHECK(!kr_device_id_valid(NULL));
}

TEST(name_filter)
{
    char out[KR_NAME_SIZE];
    kr_name_filter("Meu \"PC\" (sala): v1.0!", out, sizeof out);
    CHECK_STR_EQ(out, "Meu PC sala v10");
    kr_name_filter("Escritório ção", out, sizeof out);
    CHECK_STR_EQ(out, "Escritório ção");
    /* 32 characters, not bytes. */
    kr_name_filter("ááááááááááááááááááááááááááááááááááá", out, sizeof out);
    CHECK_STR_EQ(out, "áááááááááááááááááááááááááááááááá");
    kr_name_filter("ok\xff\xfe", out, sizeof out);
    CHECK_STR_EQ(out, "ok");
    kr_name_filter(NULL, out, sizeof out);
    CHECK_STR_EQ(out, "");
}

TEST(identity_shapes)
{
    kr_packet *p = parse("{\"type\":\"kdeconnect.identity\",\"body\":{"
                         "\"deviceId\":\"0123456789abcdef0123456789abcdef\",\"tcpPort\":1716}}");
    CHECK(p);
    CHECK(kr_identity_valid(p, KR_IDENTITY_UDP));
    CHECK(!kr_identity_valid(p, KR_IDENTITY_CONNECTION)); /* no target */
    CHECK(!kr_identity_valid(p, KR_IDENTITY_FULL));       /* no name */
    kr_packet_free(p);

    /* Android sends targetProtocolVersion as a string. */
    p = parse("{\"type\":\"kdeconnect.identity\",\"body\":{"
              "\"deviceId\":\"0123456789abcdef0123456789abcdef\",\"deviceName\":\"..\","
              "\"targetDeviceId\":\"fedcba9876543210fedcba9876543210\","
              "\"targetProtocolVersion\":\"8\",\"protocolVersion\":8}}");
    CHECK(p);
    CHECK(kr_identity_valid(p, KR_IDENTITY_CONNECTION));
    CHECK(!kr_identity_valid(p, KR_IDENTITY_FULL)); /* name empty once filtered */
    int64_t v = 0;
    CHECK(kr_packet_int(p, "targetProtocolVersion", &v) && v == 8);
    CHECK(kr_packet_int(p, "protocolVersion", &v) && v == 8);
    CHECK(!kr_packet_int(p, "deviceId", &v));
    kr_packet_free(p);

    p = parse("{\"type\":\"kdeconnect.ping\",\"body\":{"
              "\"deviceId\":\"0123456789abcdef0123456789abcdef\"}}");
    CHECK(!kr_identity_valid(p, KR_IDENTITY_UDP));
    kr_packet_free(p);
}

int main(void)
{
    RUN(parses_a_packet);
    RUN(body_is_optional_and_type_is_not);
    RUN(payload_fields);
    RUN(serializes_one_line);
    RUN(device_ids);
    RUN(name_filter);
    RUN(identity_shapes);
    return harness_report();
}
