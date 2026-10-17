/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuzz target for everything that parses packets from the network:
 * kr_packet_parse, the identity checks the handshake makes, the field
 * getters, and serializing what was parsed.
 *
 *   make fuzz && build/fuzz/fuzz_packet tests/fuzz/corpus/packet
 *
 * Without libFuzzer (make test) tests/fuzz/driver.c runs it over the
 * corpus files instead. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "packet.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    kr_packet *p = kr_packet_parse((const char *)data, size);
    if (!p)
        return 0;
    for (int k = KR_IDENTITY_UDP; k <= KR_IDENTITY_FULL; k++)
        (void)kr_identity_valid(p, (kr_identity_kind)k);
    int64_t n;
    static const char *const ints[] = {"protocolVersion", "targetProtocolVersion", "tcpPort",
                                       "timestamp", "lastModified"};
    for (size_t i = 0; i < sizeof ints / sizeof *ints; i++)
        (void)kr_packet_int(p, ints[i], &n);
    (void)kr_packet_bool(p, "pair", false);
    char name[KR_NAME_SIZE];
    kr_name_filter(kr_packet_str(p, "deviceName"), name, sizeof name);
    (void)kr_device_id_valid(kr_packet_str(p, "deviceId"));

    size_t len;
    char *s = kr_packet_serialize(p, &len);
    if (s) {
        /* What we write must parse back to the same type. */
        kr_packet *q = kr_packet_parse(s, len);
        if (!q || strcmp(q->type, p->type) != 0)
            abort();
        kr_packet_free(q);
        free(s);
    }
    kr_packet_free(p);
    return 0;
}
