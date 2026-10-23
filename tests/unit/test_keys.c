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

int main(void)
{
    RUN(keys_that_go_as_keycodes);
    RUN(what_types_something_else_goes_as_text);
    RUN(modifiers_become_the_metastate);
    return harness_report();
}
