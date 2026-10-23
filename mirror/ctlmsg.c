/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Control messages to the scrcpy-server and device messages from it
 * (ctlmsg.h), byte for byte as upstream/scrcpy/app/src/control_msg.c
 * (sc_control_msg_serialize) and device_msg.c (sc_device_msg_deserialize). */
#include "ctlmsg.h"

#include <stdlib.h>
#include <string.h>

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void w64(uint8_t *p, uint64_t v)
{
    w32(p, (uint32_t)(v >> 32));
    w32(p + 4, (uint32_t)v);
}

static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* As many bytes of utf8 as fit in max without cutting a character
 * (sc_str_utf8_truncation_index). */
static size_t utf8_cut(const char *s, size_t max)
{
    size_t len = strlen(s);
    if (len <= max)
        return len;
    len = max;
    while (len && (s[len] & 0x80) && (s[len] & 0xc0) != 0xc0)
        len--;
    return len;
}

/* 4-byte length, then the bytes (write_string). */
static size_t put_string(uint8_t *buf, const char *s, size_t max)
{
    size_t len = s ? utf8_cut(s, max) : 0;
    w32(buf, (uint32_t)len);
    if (len)
        memcpy(buf + 4, s, len);
    return 4 + len;
}

/* x, y, w, h (write_position). */
static void put_position(uint8_t *buf, int32_t x, int32_t y, uint16_t w, uint16_t h)
{
    w32(buf, (uint32_t)x);
    w32(buf + 4, (uint32_t)y);
    w16(buf + 8, w);
    w16(buf + 10, h);
}

/* [0, 1] as 16-bit fixed point (sc_float_to_u16fp). */
static uint16_t u16fp(float f)
{
    if (f <= 0)
        return 0;
    if (f >= 1)
        return 0xffff;
    uint32_t u = (uint32_t)(f * 65536.0f);
    return u >= 0xffff ? 0xffff : (uint16_t)u;
}

/* [-1, 1] as signed 16-bit fixed point (sc_float_to_i16fp). */
static int16_t i16fp(float f)
{
    if (f < -1)
        f = -1;
    if (f > 1)
        f = 1;
    int32_t i = (int32_t)(f * 32768.0f);
    if (i >= 0x7fff)
        i = 0x7fff;
    return (int16_t)i;
}

size_t kr_ctl_keycode(uint8_t *buf, int action, uint32_t keycode, uint32_t repeat, uint32_t metastate)
{
    buf[0] = KR_CTL_INJECT_KEYCODE;
    buf[1] = (uint8_t)action;
    w32(buf + 2, keycode);
    w32(buf + 6, repeat);
    w32(buf + 10, metastate);
    return 14;
}

size_t kr_ctl_text(uint8_t *buf, const char *utf8)
{
    buf[0] = KR_CTL_INJECT_TEXT;
    return 1 + put_string(buf + 1, utf8, KR_CTL_TEXT_MAX);
}

size_t kr_ctl_touch(uint8_t *buf, int action, uint64_t pointer, int32_t x, int32_t y, uint16_t w, uint16_t h,
                    float pressure, uint32_t action_button, uint32_t buttons)
{
    buf[0] = KR_CTL_INJECT_TOUCH;
    buf[1] = (uint8_t)action;
    w64(buf + 2, pointer);
    put_position(buf + 10, x, y, w, h);
    w16(buf + 22, u16fp(pressure));
    w32(buf + 24, action_button);
    w32(buf + 28, buttons);
    return 32;
}

size_t kr_ctl_scroll(uint8_t *buf, int32_t x, int32_t y, uint16_t w, uint16_t h, float hscroll, float vscroll,
                     uint32_t buttons)
{
    buf[0] = KR_CTL_INJECT_SCROLL;
    put_position(buf + 1, x, y, w, h);
    /* [-16, 16] normalized to [-1, 1], as the client does. */
    w16(buf + 13, (uint16_t)i16fp(hscroll / 16));
    w16(buf + 15, (uint16_t)i16fp(vscroll / 16));
    w32(buf + 17, buttons);
    return 21;
}

size_t kr_ctl_back_or_screen_on(uint8_t *buf, int action)
{
    buf[0] = KR_CTL_BACK_OR_SCREEN_ON;
    buf[1] = (uint8_t)action;
    return 2;
}

size_t kr_ctl_simple(uint8_t *buf, int type)
{
    buf[0] = (uint8_t)type;
    return 1;
}

size_t kr_ctl_set_clipboard(uint8_t *buf, uint64_t sequence, const char *utf8, bool paste)
{
    buf[0] = KR_CTL_SET_CLIPBOARD;
    w64(buf + 1, sequence);
    buf[9] = paste ? 1 : 0;
    return 10 + put_string(buf + 10, utf8, KR_CTL_CLIPBOARD_MAX);
}

size_t kr_ctl_set_display_power(uint8_t *buf, bool on)
{
    buf[0] = KR_CTL_SET_DISPLAY_POWER;
    buf[1] = on ? 1 : 0;
    return 2;
}

/* ---- device messages --------------------------------------------------------- */

enum { DEV_CLIPBOARD = 0, DEV_ACK_CLIPBOARD = 1, DEV_UHID_OUTPUT = 2 };
#define DEV_MAX (1u << 18) /* DEVICE_MSG_MAX_SIZE */

struct kr_devmsg {
    kr_devmsg_cbs cbs;
    void *ud;
    uint8_t *buf; /* what has arrived and is not a whole message yet */
    size_t len, cap;
    bool broken;
};

kr_devmsg *kr_devmsg_new(const kr_devmsg_cbs *cbs, void *ud)
{
    kr_devmsg *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->cbs = *cbs;
    p->ud = ud;
    return p;
}

void kr_devmsg_free(kr_devmsg *p)
{
    if (!p)
        return;
    free(p->buf);
    free(p);
}

/* The size of the whole message at buf, 0 if more must come, -1 if the
 * stream is broken. */
static long message_size(const uint8_t *b, size_t len)
{
    switch (b[0]) {
    case DEV_CLIPBOARD:
        if (len < 5)
            return 0;
        if (r32(b + 1) > DEV_MAX - 5)
            return -1;
        return len - 5 < r32(b + 1) ? 0 : (long)(5 + r32(b + 1));
    case DEV_ACK_CLIPBOARD:
        return len < 9 ? 0 : 9;
    case DEV_UHID_OUTPUT: {
        if (len < 5)
            return 0;
        size_t size = (size_t)b[3] << 8 | b[4];
        return len - 5 < size ? 0 : (long)(5 + size);
    }
    default:
        return -1;
    }
}

static void deliver(kr_devmsg *p, uint8_t *b, size_t size)
{
    if (b[0] == DEV_CLIPBOARD && p->cbs.on_clipboard) {
        size_t n = size - 5; /* a copy, to give it its NUL */
        char *text = malloc(n + 1);
        if (!text)
            return;
        memcpy(text, b + 5, n);
        text[n] = 0;
        p->cbs.on_clipboard(p->ud, text, n);
        free(text);
    } else if (b[0] == DEV_ACK_CLIPBOARD && p->cbs.on_ack) {
        p->cbs.on_ack(p->ud, (uint64_t)r32(b + 1) << 32 | r32(b + 5));
    }
    /* UHID output is for HID keyboards, which we do not create. */
}

bool kr_devmsg_feed(kr_devmsg *p, const uint8_t *data, size_t len)
{
    if (p->broken)
        return false;
    if (p->len + len > p->cap) {
        size_t cap = p->cap ? p->cap : 4096;
        while (cap < p->len + len)
            cap *= 2;
        if (cap > 2 * (size_t)DEV_MAX + 4096) {
            p->broken = true;
            return false;
        }
        uint8_t *b = realloc(p->buf, cap);
        if (!b) {
            p->broken = true;
            return false;
        }
        p->buf = b;
        p->cap = cap;
    }
    memcpy(p->buf + p->len, data, len);
    p->len += len;
    size_t off = 0;
    while (off < p->len) {
        long size = message_size(p->buf + off, p->len - off);
        if (size < 0) {
            p->broken = true;
            return false;
        }
        if (size == 0)
            break;
        deliver(p, p->buf + off, (size_t)size);
        off += (size_t)size;
    }
    if (off) {
        memmove(p->buf, p->buf + off, p->len - off);
        p->len -= off;
    }
    return true;
}
