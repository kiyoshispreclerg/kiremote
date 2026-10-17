/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "packet.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A JSON number that is an integer we can hold exactly. */
static bool num_to_int(double d, int64_t *out)
{
    if (!isfinite(d) || d != floor(d) || fabs(d) > 9007199254740992.0) /* 2^53 */
        return false;
    *out = (int64_t)d;
    return true;
}

static bool json_to_int(const cJSON *v, int64_t *out)
{
    if (cJSON_IsNumber(v))
        return num_to_int(v->valuedouble, out);
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end;
        errno = 0;
        long long n = strtoll(v->valuestring, &end, 10);
        if (errno || *end)
            return false;
        *out = n;
        return true;
    }
    return false;
}

kr_packet *kr_packet_parse(const char *data, size_t len)
{
    kr_packet *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->payload_size = -1;
    p->root = cJSON_ParseWithLength(data, len);
    if (!cJSON_IsObject(p->root))
        goto bad;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(p->root, "type");
    if (!cJSON_IsString(type) || !type->valuestring[0])
        goto bad;
    p->type = type->valuestring;

    cJSON *body = cJSON_GetObjectItemCaseSensitive(p->root, "body");
    if (!cJSON_IsObject(body)) {
        cJSON *empty = cJSON_CreateObject();
        if (!empty)
            goto bad;
        if (body)
            cJSON_ReplaceItemViaPointer(p->root, body, empty);
        else
            cJSON_AddItemToObject(p->root, "body", empty);
        body = empty;
    }
    p->body = body;

    int64_t n;
    if (json_to_int(cJSON_GetObjectItemCaseSensitive(p->root, "payloadSize"), &n))
        p->payload_size = n;
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(p->root, "payloadTransferInfo");
    if (cJSON_IsObject(info) &&
        json_to_int(cJSON_GetObjectItemCaseSensitive(info, "port"), &n) && n > 0 && n < 65536)
        p->payload_port = (int)n;
    return p;

bad:
    cJSON_Delete(p->root);
    free(p);
    return NULL;
}

kr_packet *kr_packet_new(const char *type)
{
    kr_packet *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->payload_size = -1;
    p->root = cJSON_CreateObject();
    cJSON *t = p->root ? cJSON_AddStringToObject(p->root, "type", type) : NULL;
    p->body = t ? cJSON_AddObjectToObject(p->root, "body") : NULL;
    if (!p->body) {
        cJSON_Delete(p->root);
        free(p);
        return NULL;
    }
    p->type = t->valuestring;
    return p;
}

void kr_packet_free(kr_packet *p)
{
    if (!p)
        return;
    cJSON_Delete(p->root);
    free(p);
}

bool kr_packet_set_payload(kr_packet *p, int64_t size, int port)
{
    p->payload_size = size;
    p->payload_port = port;
    return true;
}

char *kr_packet_serialize(const kr_packet *p, size_t *len)
{
    /* Same key order as the upstream: id, type, body, payload. */
    cJSON *out = cJSON_CreateObject();
    if (!out)
        return NULL;
    char *s = NULL;
    cJSON *body = cJSON_Duplicate(p->body, true);
    if (!body || !cJSON_AddNumberToObject(out, "id", (double)now_ms()) ||
        !cJSON_AddStringToObject(out, "type", p->type)) {
        cJSON_Delete(body);
        goto done;
    }
    cJSON_AddItemToObject(out, "body", body);
    if (p->payload_size >= 0 || p->payload_port) {
        cJSON *info;
        if (!cJSON_AddNumberToObject(out, "payloadSize", (double)p->payload_size) ||
            !(info = cJSON_AddObjectToObject(out, "payloadTransferInfo")))
            goto done;
        if (p->payload_port && !cJSON_AddNumberToObject(info, "port", p->payload_port))
            goto done;
    }
    char *json = cJSON_PrintUnformatted(out);
    if (!json)
        goto done;
    size_t n = strlen(json);
    s = malloc(n + 2);
    if (s) {
        memcpy(s, json, n);
        s[n] = '\n';
        s[n + 1] = '\0';
        if (len)
            *len = n + 1;
    }
    cJSON_free(json);
done:
    cJSON_Delete(out);
    return s;
}

bool kr_device_id_valid(const char *id)
{
    if (!id)
        return false;
    size_t n = 0;
    for (const char *c = id; *c; c++, n++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
        if (!ok || n >= 38)
            return false;
    }
    return n >= 32;
}

/* Length of the UTF-8 sequence starting at s, or 0 if it is not valid. */
static size_t utf8_len(const unsigned char *s)
{
    size_t n;
    if (s[0] < 0x80)
        return 1;
    else if ((s[0] & 0xE0) == 0xC0 && s[0] >= 0xC2)
        n = 2;
    else if ((s[0] & 0xF0) == 0xE0)
        n = 3;
    else if ((s[0] & 0xF8) == 0xF0 && s[0] <= 0xF4)
        n = 4;
    else
        return 0;
    for (size_t i = 1; i < n; i++)
        if ((s[i] & 0xC0) != 0x80)
            return 0;
    return n;
}

void kr_name_filter(const char *in, char *out, size_t size)
{
    size_t o = 0;
    int chars = 0;
    if (size == 0)
        return;
    const unsigned char *s = (const unsigned char *)(in ? in : "");
    while (*s && chars < 32) {
        size_t n = utf8_len(s);
        if (n == 0) {
            s++;
            continue;
        }
        if (n == 1 && strchr("\"',;:.!?()[]<>", *s)) {
            s++;
            continue;
        }
        if (o + n >= size)
            break;
        memcpy(out + o, s, n);
        o += n;
        s += n;
        chars++;
    }
    out[o] = '\0';
}

/* Not empty and not only white space, like QString::isEmpty after the
 * filter (Android also refuses blank names). */
static bool name_usable(const char *name)
{
    char buf[KR_NAME_SIZE];
    kr_name_filter(name, buf, sizeof buf);
    for (const char *c = buf; *c; c++)
        if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r')
            return true;
    return false;
}

bool kr_identity_valid(const kr_packet *p, kr_identity_kind kind)
{
    if (strcmp(p->type, KR_PACKET_IDENTITY) != 0 ||
        !kr_device_id_valid(kr_packet_str(p, "deviceId")))
        return false;
    switch (kind) {
    case KR_IDENTITY_UDP:
        return true;
    case KR_IDENTITY_CONNECTION:
        return kr_device_id_valid(kr_packet_str(p, "targetDeviceId"));
    case KR_IDENTITY_FULL:
        return name_usable(kr_packet_str(p, "deviceName"));
    }
    return false;
}

bool kr_packet_int(const kr_packet *p, const char *key, int64_t *out)
{
    return json_to_int(cJSON_GetObjectItemCaseSensitive(p->body, key), out);
}

const char *kr_packet_str(const kr_packet *p, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p->body, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

bool kr_packet_bool(const kr_packet *p, const char *key, bool fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p->body, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}
