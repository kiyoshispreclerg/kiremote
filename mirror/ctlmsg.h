/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CTLMSG_H
#define KR_CTLMSG_H

/* The scrcpy-server's control socket (docs/PROTOCOL.md, "Espelhamento"):
 * the messages we send, serialized into a buffer, and the ones the phone
 * sends back, parsed as the bytes arrive. No I/O. Read in upstream/scrcpy:
 * app/src/control_msg.c, device_msg.c and android/input.h, keycodes.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Message types, in the server's order (sc_control_msg_type). */
enum {
    KR_CTL_INJECT_KEYCODE = 0,
    KR_CTL_INJECT_TEXT = 1,
    KR_CTL_INJECT_TOUCH = 2,
    KR_CTL_INJECT_SCROLL = 3,
    KR_CTL_BACK_OR_SCREEN_ON = 4,
    KR_CTL_EXPAND_NOTIFICATIONS = 5,
    KR_CTL_EXPAND_SETTINGS = 6,
    KR_CTL_COLLAPSE_PANELS = 7,
    KR_CTL_GET_CLIPBOARD = 8,
    KR_CTL_SET_CLIPBOARD = 9,
    KR_CTL_SET_DISPLAY_POWER = 10,
    KR_CTL_ROTATE_DEVICE = 11,
};

/* Android's values (android/input.h). */
enum { KR_AKEY_DOWN = 0, KR_AKEY_UP = 1 };
enum { KR_AMOTION_DOWN = 0, KR_AMOTION_UP = 1, KR_AMOTION_MOVE = 2, KR_AMOTION_HOVER_MOVE = 7 };
enum {
    KR_ABUTTON_PRIMARY = 1 << 0,
    KR_ABUTTON_SECONDARY = 1 << 1,
    KR_ABUTTON_TERTIARY = 1 << 2,
    KR_ABUTTON_BACK = 1 << 3,
    KR_ABUTTON_FORWARD = 1 << 4,
};
#define KR_POINTER_MOUSE UINT64_C(0xFFFFFFFFFFFFFFFF) /* -1, SC_POINTER_ID_MOUSE */

/* The longest messages we build: text is cut (at a character boundary)
 * to what the server takes. */
#define KR_CTL_TEXT_MAX 300
#define KR_CTL_MAX_SIZE (1 << 18) /* SC_CONTROL_MSG_MAX_SIZE */
#define KR_CTL_CLIPBOARD_MAX (KR_CTL_MAX_SIZE - 14)

/* Each returns the bytes written to buf, which must hold the message
 * (KR_CTL_MAX_SIZE always does). Positions are in the video's pixels,
 * with the video size they refer to (the server drops events for another
 * size, as after a rotation). */
size_t kr_ctl_keycode(uint8_t *buf, int action, uint32_t keycode, uint32_t repeat, uint32_t metastate);
size_t kr_ctl_text(uint8_t *buf, const char *utf8);
size_t kr_ctl_touch(uint8_t *buf, int action, uint64_t pointer, int32_t x, int32_t y, uint16_t w, uint16_t h,
                    float pressure, uint32_t action_button, uint32_t buttons);
/* hscroll, vscroll: in [-16, 16] (one wheel notch is 1). */
size_t kr_ctl_scroll(uint8_t *buf, int32_t x, int32_t y, uint16_t w, uint16_t h, float hscroll, float vscroll,
                     uint32_t buttons);
size_t kr_ctl_back_or_screen_on(uint8_t *buf, int action);
/* Types with nothing after them (expand, collapse, rotate). */
size_t kr_ctl_simple(uint8_t *buf, int type);
size_t kr_ctl_set_clipboard(uint8_t *buf, uint64_t sequence, const char *utf8, bool paste);
size_t kr_ctl_set_display_power(uint8_t *buf, bool on);

/* ---- what the phone sends back -------------------------------------------- */

typedef struct {
    /* The phone's clipboard changed (text, NUL-terminated, not checked). */
    void (*on_clipboard)(void *ud, const char *text, size_t len);
    /* It applied our SET_CLIPBOARD with that sequence. */
    void (*on_ack)(void *ud, uint64_t sequence);
} kr_devmsg_cbs;

typedef struct kr_devmsg kr_devmsg;

kr_devmsg *kr_devmsg_new(const kr_devmsg_cbs *cbs, void *ud);
void kr_devmsg_free(kr_devmsg *p);
/* false once the stream is broken (unknown type, too long): the client
 * cannot find the next message, as upstream. */
bool kr_devmsg_feed(kr_devmsg *p, const uint8_t *data, size_t len);

#endif
