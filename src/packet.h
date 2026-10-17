/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_PACKET_H
#define KR_PACKET_H

/* KDE Connect network packets: one compact JSON object per line,
 *
 *   {"id":<ms>,"type":"kdeconnect.x","body":{...},
 *    "payloadSize":N,"payloadTransferInfo":{"port":P}}
 *
 * plus the checks the upstream makes on identity packets (device ids,
 * device names). Everything here parses input from the network, so it is
 * also what tests/fuzz exercises. See docs/PROTOCOL.md.
 *
 * A kr_packet owns its JSON tree; type and body point into it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define KR_PROTOCOL_VERSION 8

/* Identity packets: up to 8192 bytes (MAX_IDENTITY_PACKET_SIZE). Others:
 * up to 32 MiB (MAX_PACKET_SIZE). */
#define KR_MAX_IDENTITY_SIZE 8192
#define KR_MAX_PACKET_SIZE (32u * 1024 * 1024)

#define KR_PACKET_IDENTITY "kdeconnect.identity"
#define KR_PACKET_PAIR "kdeconnect.pair"

/* Device ids are 32 to 38 of [a-zA-Z0-9_-]: up to 38 chars plus NUL. */
#define KR_ID_SIZE 40
/* Names are cut to 32 characters, up to 4 UTF-8 bytes each. */
#define KR_NAME_SIZE (32 * 4 + 1)

typedef struct {
    cJSON *root;
    const char *type; /* never NULL or empty */
    cJSON *body;      /* always an object */
    /* payloadSize: -1 when the packet has none (an unknown size in the
     * upstream is also -1, but then payloadTransferInfo is there). */
    int64_t payload_size;
    int payload_port; /* payloadTransferInfo.port, 0 if absent */
} kr_packet;

/* Parses one packet (len bytes, a trailing newline is fine). NULL if it is
 * not a JSON object with a non-empty "type". A missing or non-object body
 * becomes an empty one, as in the upstream. */
kr_packet *kr_packet_parse(const char *data, size_t len);

/* A new packet with an empty body; NULL only without memory. */
kr_packet *kr_packet_new(const char *type);
void kr_packet_free(kr_packet *p);

/* Marks the packet as carrying a payload of size bytes, served on port. */
bool kr_packet_set_payload(kr_packet *p, int64_t size, int port);

/* The line to send: compact JSON with "id" (now, in ms since the epoch)
 * and a final "\n". The caller frees it; NULL without memory. */
char *kr_packet_serialize(const kr_packet *p, size_t *len);

/* ^[a-zA-Z0-9_-]{32,38}$ */
bool kr_device_id_valid(const char *id);

/* The upstream's filterName: drops "',;:.!?()[]<> and keeps the first 32
 * characters. Invalid UTF-8 bytes are dropped too. out gets "" for NULL. */
void kr_name_filter(const char *in, char *out, size_t size);

/* The three shapes of kdeconnect.identity (docs/PROTOCOL.md). */
typedef enum {
    KR_IDENTITY_UDP,        /* broadcast: deviceId (and tcpPort) */
    KR_IDENTITY_CONNECTION, /* plain text on TCP: deviceId, targetDeviceId */
    KR_IDENTITY_FULL,       /* inside TLS: deviceId, a usable deviceName */
} kr_identity_kind;

bool kr_identity_valid(const kr_packet *p, kr_identity_kind kind);

/* Integer field of the body. Numbers and strings holding an integer are
 * accepted (Android sends targetProtocolVersion as a string). */
bool kr_packet_int(const kr_packet *p, const char *key, int64_t *out);
/* String field of the body, or NULL. */
const char *kr_packet_str(const kr_packet *p, const char *key);
/* Boolean field of the body, or fallback when absent or not a boolean. */
bool kr_packet_bool(const kr_packet *p, const char *key, bool fallback);

#endif
