/* SPDX-License-Identifier: GPL-3.0-or-later */
/* X keysyms and modifiers to Android's (keys.h). The keycodes are from
 * upstream/scrcpy/app/src/android/keycodes.h, the table is
 * convert_keycode in keyboard_sdk.c (its SDL keys, here X keysyms). */
#include "keys.h"

#include <X11/X.h>
#include <X11/keysym.h>
#include <stdbool.h>
#include <stddef.h>

static const struct {
    unsigned long sym;
    uint32_t code;
} map[] = {
    {XK_Return, 66},      {XK_KP_Enter, 160},   {XK_Escape, 111},     {XK_BackSpace, 67},
    {XK_Tab, 61},         {XK_ISO_Left_Tab, 61}, {XK_Prior, 92},      {XK_Delete, 112},
    {XK_Home, 122},       {XK_End, 123},        {XK_Next, 93},        {XK_Right, 22},
    {XK_Left, 21},        {XK_Down, 20},        {XK_Up, 19},          {XK_Insert, 124},
    {XK_Control_L, 113},  {XK_Control_R, 114},  {XK_Shift_L, 59},     {XK_Shift_R, 60},
    {XK_Alt_L, 57},       {XK_Alt_R, 58},       {XK_Super_L, 117},    {XK_Super_R, 118},
    /* The keypad without Num Lock: its navigation keys. */
    {XK_KP_Insert, 124},  {XK_KP_End, 123},     {XK_KP_Down, 20},     {XK_KP_Next, 93},
    {XK_KP_Left, 21},     {XK_KP_Right, 22},    {XK_KP_Home, 122},    {XK_KP_Up, 19},
    {XK_KP_Prior, 92},    {XK_KP_Delete, 112},
};

uint32_t kr_keys_keycode(unsigned long sym)
{
    if (sym >= XK_a && sym <= XK_z)
        return 29 + (uint32_t)(sym - XK_a); /* AKEYCODE_A.. */
    if (sym >= XK_A && sym <= XK_Z)
        return 29 + (uint32_t)(sym - XK_A);
    if (sym == XK_space)
        return 62;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (map[i].sym == sym)
            return map[i].code;
    return 0;
}

uint32_t kr_keys_metastate(unsigned state)
{
    uint32_t m = 0;
    if (state & ShiftMask)
        m |= 0x40 | 0x01; /* AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_ON */
    if (state & ControlMask)
        m |= 0x2000 | 0x1000;
    if (state & Mod1Mask) /* Alt */
        m |= 0x10 | 0x02;
    if (state & Mod4Mask) /* Super */
        m |= 0x20000 | 0x10000;
    if (state & LockMask)
        m |= 0x100000;
    if (state & Mod2Mask) /* Num Lock */
        m |= 0x200000;
    return m;
}

kr_shortcut kr_keys_shortcut(unsigned long sym, unsigned state)
{
    if (!(state & (Mod1Mask | Mod4Mask)) || (state & ControlMask))
        return KR_SC_NONE;
    bool shift = state & ShiftMask;
    if (sym >= XK_A && sym <= XK_Z)
        sym += XK_a - XK_A;
    switch (sym) {
    case XK_n:
        return shift ? KR_SC_COLLAPSE : KR_SC_NOTIFICATIONS;
    case XK_o:
        return shift ? KR_SC_SCREEN_ON : KR_SC_SCREEN_OFF;
    default:
        break;
    }
    if (shift)
        return KR_SC_NONE;
    switch (sym) {
    case XK_h:
        return KR_SC_HOME;
    case XK_b:
    case XK_BackSpace:
        return KR_SC_BACK;
    case XK_s:
        return KR_SC_APP_SWITCH;
    case XK_m:
        return KR_SC_MENU;
    case XK_p:
        return KR_SC_POWER;
    case XK_Up:
        return KR_SC_VOLUME_UP;
    case XK_Down:
        return KR_SC_VOLUME_DOWN;
    case XK_r:
        return KR_SC_ROTATE;
    case XK_f:
        return KR_SC_FULLSCREEN;
    default:
        return KR_SC_NONE;
    }
}
