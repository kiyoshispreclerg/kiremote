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

#endif
