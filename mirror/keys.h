/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_KEYS_H
#define KR_KEYS_H

/* X keys to Android's, as scrcpy's default "mixed" mode does
 * (upstream/scrcpy/app/src/keyboard_sdk.c): navigation keys, Enter, the
 * modifiers, letters and space go as Android keycodes with the modifiers
 * as metastate; everything else that types something goes as text (the
 * caller looks the text up with its input method). Only constants from
 * the X headers, no X connection. */

#include <stdint.h>

/* The Android keycode for an X keysym, 0 if it is not sent as a key. */
uint32_t kr_keys_keycode(unsigned long keysym);
/* Android's metastate for an X modifier state (ShiftMask...). X does not
 * say left from right: both are the left ones. */
uint32_t kr_keys_metastate(unsigned state);

/* Alt+key or Super+key does something to the phone or the window instead of
 * being typed (scrcpy's shortcuts, upstream/scrcpy/app/src/input_manager.c:
 * h home, b or Backspace back, s recent apps, m menu, p power, Up and Down
 * volume, n notifications (with Shift: close the panels), r rotate, o screen
 * off (with Shift: on), f fullscreen). The Shift variants of the others
 * are not shortcuts, as there. */
typedef enum {
    KR_SC_NONE = 0,
    KR_SC_HOME,
    KR_SC_BACK,
    KR_SC_APP_SWITCH,
    KR_SC_MENU,
    KR_SC_POWER,
    KR_SC_VOLUME_UP,
    KR_SC_VOLUME_DOWN,
    KR_SC_NOTIFICATIONS,
    KR_SC_COLLAPSE,
    KR_SC_ROTATE,
    KR_SC_SCREEN_OFF,
    KR_SC_SCREEN_ON,
    KR_SC_FULLSCREEN,
} kr_shortcut;

/* KR_SC_NONE if the key with this X modifier state is no shortcut (Ctrl
 * held makes it none: Ctrl+Alt+key belongs to the window manager). */
kr_shortcut kr_keys_shortcut(unsigned long keysym, unsigned state);

#endif
