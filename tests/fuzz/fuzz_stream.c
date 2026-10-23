/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuzz target for the scrcpy-server streams (mirror/stream.c), what
 * kiremote-mirror reads from the phone. The first byte picks the kind of
 * stream and the size of the pieces the rest arrives in.
 *
 *   make fuzz && build/fuzz/fuzz_stream tests/fuzz/corpus/stream */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stream.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void on_name(void *ud, const char *name)
{
    if (strlen(name) >= KR_STREAM_NAME_SIZE)
        abort();
    (void)ud;
}

static void on_packet(void *ud, const uint8_t *data, size_t len, int64_t pts, bool key)
{
    volatile uint8_t sink = 0;
    (void)ud;
    (void)pts;
    (void)key;
    if (!len || len > 2 * (size_t)KR_STREAM_MAX_PACKET)
        abort();
    sink ^= data[0] ^ data[len - 1]; /* the whole packet is readable */
    (void)sink;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!size)
        return 0;
    uint8_t mode = data[0];
    data++;
    size--;
    static const kr_stream_cbs cbs = {on_name, NULL, NULL, on_packet};
    kr_stream *s = kr_stream_new(mode & 1, mode & 2, &cbs, NULL);
    if (!s)
        return 0;
    size_t chunk = (size_t)(mode >> 2) + 1;
    while (size) {
        size_t n = size < chunk ? size : chunk;
        bool ok = kr_stream_feed(s, data, n);
        if (!ok && !kr_stream_error(s))
            abort();
        data += n;
        size -= n;
    }
    kr_stream_free(s);
    return 0;
}
