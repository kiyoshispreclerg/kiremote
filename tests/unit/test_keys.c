/* SPDX-License-Identifier: GPL-3.0-or-later */
/* X keys to Android's, against keyboard_sdk.c's mixed mode. */
#include <X11/X.h>
#include <X11/keysym.h>

#include "../harness.h"
#include "keys.h"

TEST(keys_that_go_as_keycodes)
{
    CHECK_INT_EQ(kr_keys_keycode(XK_a), 29);
    CHECK_INT_EQ(kr_keys_keycode(XK_Z), 54);
    CHECK_INT_EQ(kr_keys_keycode(XK_space), 62);
    CHECK_INT_EQ(kr_keys_keycode(XK_Return), 66);
    CHECK_INT_EQ(kr_keys_keycode(XK_BackSpace), 67);
    CHECK_INT_EQ(kr_keys_keycode(XK_Up), 19);
    CHECK_INT_EQ(kr_keys_keycode(XK_KP_Home), 122);
    CHECK_INT_EQ(kr_keys_keycode(XK_Control_R), 114);
}

TEST(what_types_something_else_goes_as_text)
{
    CHECK_INT_EQ(kr_keys_keycode(XK_1), 0);
    CHECK_INT_EQ(kr_keys_keycode(XK_comma), 0);
    CHECK_INT_EQ(kr_keys_keycode(XK_ccedilla), 0);
    CHECK_INT_EQ(kr_keys_keycode(XK_KP_5), 0);
    CHECK_INT_EQ(kr_keys_keycode(XK_dead_acute), 0);
}

TEST(modifiers_become_the_metastate)
{
    CHECK_INT_EQ(kr_keys_metastate(0), 0);
    CHECK_INT_EQ(kr_keys_metastate(ShiftMask), 0x41);
    CHECK_INT_EQ(kr_keys_metastate(ControlMask | ShiftMask), 0x3041);
    CHECK_INT_EQ(kr_keys_metastate(Mod1Mask), 0x12);
    CHECK_INT_EQ(kr_keys_metastate(Mod4Mask | LockMask | Mod2Mask), 0x330000);
}

TEST(alt_or_super_make_shortcuts)
{
    CHECK_INT_EQ(kr_keys_shortcut(XK_h, Mod1Mask), KR_SC_HOME);
    CHECK_INT_EQ(kr_keys_shortcut(XK_h, Mod4Mask), KR_SC_HOME);
    CHECK_INT_EQ(kr_keys_shortcut(XK_H, Mod1Mask | LockMask), KR_SC_HOME); /* Caps Lock */
    CHECK_INT_EQ(kr_keys_shortcut(XK_b, Mod1Mask | Mod2Mask), KR_SC_BACK); /* Num Lock */
    CHECK_INT_EQ(kr_keys_shortcut(XK_BackSpace, Mod1Mask), KR_SC_BACK);
    CHECK_INT_EQ(kr_keys_shortcut(XK_Up, Mod1Mask), KR_SC_VOLUME_UP);
    CHECK_INT_EQ(kr_keys_shortcut(XK_Down, Mod1Mask), KR_SC_VOLUME_DOWN);
    CHECK_INT_EQ(kr_keys_shortcut(XK_n, Mod1Mask), KR_SC_NOTIFICATIONS);
    CHECK_INT_EQ(kr_keys_shortcut(XK_N, Mod1Mask | ShiftMask), KR_SC_COLLAPSE);
    CHECK_INT_EQ(kr_keys_shortcut(XK_o, Mod1Mask), KR_SC_SCREEN_OFF);
    CHECK_INT_EQ(kr_keys_shortcut(XK_O, Mod1Mask | ShiftMask), KR_SC_SCREEN_ON);
    CHECK_INT_EQ(kr_keys_shortcut(XK_r, Mod1Mask), KR_SC_ROTATE);
    CHECK_INT_EQ(kr_keys_shortcut(XK_f, Mod1Mask), KR_SC_FULLSCREEN);
}

TEST(everything_else_is_typed)
{
    CHECK_INT_EQ(kr_keys_shortcut(XK_h, 0), KR_SC_NONE);
    CHECK_INT_EQ(kr_keys_shortcut(XK_h, ShiftMask), KR_SC_NONE);
    CHECK_INT_EQ(kr_keys_shortcut(XK_h, Mod1Mask | ControlMask), KR_SC_NONE); /* the window manager's */
    CHECK_INT_EQ(kr_keys_shortcut(XK_h, Mod1Mask | ShiftMask), KR_SC_NONE);
    CHECK_INT_EQ(kr_keys_shortcut(XK_x, Mod1Mask), KR_SC_NONE);
    CHECK_INT_EQ(kr_keys_shortcut(XK_Return, Mod1Mask), KR_SC_NONE);
}

int main(void)
{
    RUN(keys_that_go_as_keycodes);
    RUN(what_types_something_else_goes_as_text);
    RUN(modifiers_become_the_metastate);
    RUN(alt_or_super_make_shortcuts);
    RUN(everything_else_is_typed);
    return harness_report();
}
