/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuzz target for the messages the scrcpy-server sends on the control
 * socket (mirror/ctlmsg.c, kr_devmsg): the phone's clipboard and acks. The
 * first byte picks the size of the pieces the rest arrives in.
 *
 *   make fuzz && build/fuzz/fuzz_devmsg tests/fuzz/corpus/devmsg */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ctlmsg.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void on_clipboard(void *ud, const char *text, size_t len)
{
    (void)ud;
    if (text[len] != 0 || len > KR_CTL_MAX_SIZE)
        abort();
}

static void on_ack(void *ud, uint64_t seq)
{
    (void)ud;
    (void)seq;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!size)
        return 0;
    size_t chunk = (size_t)data[0] + 1;
    data++;
    size--;
    static const kr_devmsg_cbs cbs = {on_clipboard, on_ack};
    kr_devmsg *p = kr_devmsg_new(&cbs, NULL);
    if (!p)
        return 0;
    while (size) {
        size_t n = size < chunk ? size : chunk;
        if (!kr_devmsg_feed(p, data, n))
            break;
        data += n;
        size -= n;
    }
    kr_devmsg_free(p);
    return 0;
}
