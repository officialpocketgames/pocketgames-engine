// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part F: XInput's keystroke edges (Part D4), through the edge detector shared by the
// HID path and the host slots (pe/dlls/xinput1_3/xinput_keystroke.h). Needs Wine 11.4's
// include/ for windef.h, winerror.h and xinput.h.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winerror.h"
#include "xinput.h"

#include "xinput_keystroke.h"
#include "test_common.h"

static XINPUT_GAMEPAD cur, last;
static XINPUT_KEYSTROKE ks;

static DWORD next(DWORD index)
{
    memset(&ks, 0xcc, sizeof(ks));
    return xinput_keystroke_edge(index, &cur, &last, &ks);
}

static void expect(DWORD index, WORD vk, WORD flags)
{
    CHECK_EQ(next(index), ERROR_SUCCESS);
    CHECK_EQ(ks.VirtualKey, vk);
    CHECK_EQ(ks.Flags, flags);
    CHECK_EQ(ks.UserIndex, index);
    CHECK_EQ(ks.Unicode, 0);
    CHECK_EQ(ks.HidCode, 0);
}

static void expect_empty(DWORD index)
{
    CHECK_EQ(next(index), ERROR_EMPTY);
}

int main(void)
{
    memset(&cur, 0, sizeof(cur));
    memset(&last, 0, sizeof(last));
    expect_empty(0);

    /* a button: down once, then nothing, then up */
    cur.wButtons = XINPUT_GAMEPAD_A;
    expect(0, VK_PAD_A, XINPUT_KEYSTROKE_KEYDOWN);
    expect_empty(0);
    cur.wButtons = 0;
    expect(0, VK_PAD_A, XINPUT_KEYSTROKE_KEYUP);
    expect_empty(0);

    /* two at once come one per call, in the table's order (d-pad before face buttons) */
    cur.wButtons = XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_UP;
    expect(2, VK_PAD_DPAD_UP, XINPUT_KEYSTROKE_KEYDOWN);
    expect(2, VK_PAD_B, XINPUT_KEYSTROKE_KEYDOWN);
    expect_empty(2);
    cur.wButtons = XINPUT_GAMEPAD_B;
    expect(2, VK_PAD_DPAD_UP, XINPUT_KEYSTROKE_KEYUP);
    cur.wButtons = 0;
    expect(2, VK_PAD_B, XINPUT_KEYSTROKE_KEYUP);

    /* the guide button sends nothing */
    cur.wButtons = 0x0400;
    expect_empty(0);
    cur.wButtons = 0;
    last.wButtons = 0;

    /* triggers: past 30 is down */
    cur.bLeftTrigger = 30;
    expect_empty(1);
    cur.bLeftTrigger = 31;
    expect(1, VK_PAD_LTRIGGER, XINPUT_KEYSTROKE_KEYDOWN);
    expect_empty(1);
    cur.bLeftTrigger = 200;
    expect_empty(1);
    cur.bLeftTrigger = 0;
    expect(1, VK_PAD_LTRIGGER, XINPUT_KEYSTROKE_KEYUP);
    cur.bRightTrigger = 255;
    expect(1, VK_PAD_RTRIGGER, XINPUT_KEYSTROKE_KEYDOWN);
    cur.bRightTrigger = 0;
    expect(1, VK_PAD_RTRIGGER, XINPUT_KEYSTROKE_KEYUP);

    /* left stick: right, then up-right (up of the old direction first), then centre */
    cur.sThumbLX = 20000;
    expect_empty(0);                                   /* not past 20000 */
    cur.sThumbLX = 25000;
    expect(0, VK_PAD_LTHUMB_RIGHT, XINPUT_KEYSTROKE_KEYDOWN);
    expect_empty(0);
    cur.sThumbLY = 25000;
    expect(0, VK_PAD_LTHUMB_RIGHT, XINPUT_KEYSTROKE_KEYUP);
    expect(0, VK_PAD_LTHUMB_UPRIGHT, XINPUT_KEYSTROKE_KEYDOWN);
    cur.sThumbLX = cur.sThumbLY = 0;
    expect(0, VK_PAD_LTHUMB_UPRIGHT, XINPUT_KEYSTROKE_KEYUP);
    expect_empty(0);

    /* right stick down-left */
    cur.sThumbRX = -32768;
    cur.sThumbRY = -32768;
    expect(3, VK_PAD_RTHUMB_DOWNLEFT, XINPUT_KEYSTROKE_KEYDOWN);
    cur.sThumbRX = 0;
    expect(3, VK_PAD_RTHUMB_DOWNLEFT, XINPUT_KEYSTROKE_KEYUP);
    expect(3, VK_PAD_RTHUMB_DOWN, XINPUT_KEYSTROKE_KEYDOWN);
    cur.sThumbRY = 0;
    expect(3, VK_PAD_RTHUMB_DOWN, XINPUT_KEYSTROKE_KEYUP);

    /* buttons take precedence over triggers and sticks in the same call */
    cur.wButtons = XINPUT_GAMEPAD_START;
    cur.bLeftTrigger = 100;
    cur.sThumbLX = -30000;
    expect(0, VK_PAD_START, XINPUT_KEYSTROKE_KEYDOWN);
    expect(0, VK_PAD_LTRIGGER, XINPUT_KEYSTROKE_KEYDOWN);
    expect(0, VK_PAD_LTHUMB_LEFT, XINPUT_KEYSTROKE_KEYDOWN);
    expect_empty(0);

    /* a zeroed state (XInputEnable(FALSE), or a fresh edge memory) releases everything */
    memset(&cur, 0, sizeof(cur));
    expect(0, VK_PAD_START, XINPUT_KEYSTROKE_KEYUP);
    expect(0, VK_PAD_LTRIGGER, XINPUT_KEYSTROKE_KEYUP);
    expect(0, VK_PAD_LTHUMB_LEFT, XINPUT_KEYSTROKE_KEYUP);
    expect_empty(0);

    return test_report("test_keystroke");
}
