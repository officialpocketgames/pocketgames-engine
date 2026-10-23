// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 8e: Windows.Gaming.Input's host-slot provider
// (pe/dlls/windows.gaming.input/wgi_host.h, used by the modified provider.c): which paths name
// a slot, and the state and vibration it answers. The provider's state is then turned into a
// GamepadReading by upstream's gamepad.c (gamepad_GetCurrentReading), whose formulas are
// repeated below, so the test checks what a WGI game reads against the XInput state.
// Needs Wine 11.4's include/ for windef.h and xinput.h.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winerror.h"
#include "xinput.h"

#include "wgi_host.h"
#include "test_common.h"

/* GamepadButtons (windows.gaming.input.idl) */
#define GB_MENU   0x0001
#define GB_VIEW   0x0002
#define GB_A      0x0004
#define GB_B      0x0008
#define GB_X      0x0010
#define GB_Y      0x0020
#define GB_UP     0x0040
#define GB_DOWN   0x0080
#define GB_LEFT   0x0100
#define GB_RIGHT  0x0200
#define GB_LS     0x0400
#define GB_RS     0x0800
#define GB_LTHUMB 0x1000
#define GB_RTHUMB 0x2000

struct reading
{
    unsigned int buttons;
    double lx, ly, rx, ry, lt, rt;
};

/* upstream gamepad.c's mapping of a provider state to a GamepadReading */
static struct reading gamepad_reading(const double *axes, const unsigned char *buttons, int sw)
{
    struct reading r;

    memset(&r, 0, sizeof(r));
    if (buttons[0]) r.buttons |= GB_A;
    if (buttons[1]) r.buttons |= GB_B;
    if (buttons[2]) r.buttons |= GB_X;
    if (buttons[3]) r.buttons |= GB_Y;
    if (buttons[4]) r.buttons |= GB_LS;
    if (buttons[5]) r.buttons |= GB_RS;
    if (buttons[6]) r.buttons |= GB_VIEW;
    if (buttons[7]) r.buttons |= GB_MENU;
    if (buttons[8]) r.buttons |= GB_LTHUMB;
    if (buttons[9]) r.buttons |= GB_RTHUMB;
    if (sw == 1 || sw == 2 || sw == 8) r.buttons |= GB_UP;
    if (sw == 5 || sw == 4 || sw == 6) r.buttons |= GB_DOWN;
    if (sw == 3 || sw == 2 || sw == 4) r.buttons |= GB_RIGHT;
    if (sw == 7 || sw == 8 || sw == 6) r.buttons |= GB_LEFT;
    r.lx = 2. * axes[1] - 1.;
    r.ly = 2. * axes[0] - 1.;
    r.lt = axes[4];
    r.rx = 2. * axes[3] - 1.;
    r.ry = 2. * axes[2] - 1.;
    r.rt = axes[5];
    return r;
}

static struct reading read_pad(WORD buttons, SHORT lx, SHORT ly, SHORT rx, SHORT ry, BYTE lt, BYTE rt)
{
    XINPUT_GAMEPAD pad = { buttons, lt, rt, lx, ly, rx, ry };
    double axes[WGI_HOST_AXES];
    unsigned char b[WGI_HOST_BUTTONS];
    int sw = -1;

    wgi_host_map(&pad, axes, b, &sw);
    CHECK(sw >= 0 && sw <= 8);
    return gamepad_reading(axes, b, sw);
}

static int near(double a, double b) { return a - b < 1e-4 && b - a < 1e-4; }

static void test_paths(void)
{
    static const WCHAR slot0[] = L"\\\\?\\HID#VID_045E&PID_028E&IG_00#9&4d616465&1&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    static const WCHAR slot3[] = L"\\\\?\\hid#vid_045e&pid_028e&ig_03#9&4d616465&1&0003#{4D1E55B2-F16F-11CF-88CB-001111000030}";
    static const WCHAR mixed[] = L"\\\\?\\HID#VID_045E&PID_028E&IG_01#9&4d616465&1&0002#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    static const WCHAR slot4[] = L"\\\\?\\HID#VID_045E&PID_028E&IG_04#9&4d616465&1&0004#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    static const WCHAR longer[] = L"\\\\?\\HID#VID_045E&PID_028E&IG_00#9&4d616465&1&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}x";
    static const WCHAR real[] = L"\\\\?\\HID#VID_045E&PID_028E&IG_00#7&1a2b3c4d&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    static const WCHAR dualsense[] = L"\\\\?\\HID#VID_054C&PID_0CE6&MI_03#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    static const WCHAR shorter[] = L"\\\\?\\HID#VID_045E&PID_028E&IG_0";

    CHECK_EQ(wgi_host_slot_from_path(slot0), 0);
    CHECK_EQ(wgi_host_slot_from_path(slot3), 3);     /* case doesn't matter */
    CHECK_EQ(wgi_host_slot_from_path(mixed), -1);    /* IG_01 with instance 0002 */
    CHECK_EQ(wgi_host_slot_from_path(slot4), -1);
    CHECK_EQ(wgi_host_slot_from_path(longer), -1);
    CHECK_EQ(wgi_host_slot_from_path(real), -1);     /* someone else's XInput pad: upstream's path */
    CHECK_EQ(wgi_host_slot_from_path(dualsense), -1);
    CHECK_EQ(wgi_host_slot_from_path(shorter), -1);
    CHECK_EQ(wgi_host_slot_from_path(NULL), -1);
}

static void test_state(void)
{
    struct reading r;

    /* at rest */
    r = read_pad(0, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(r.buttons, 0);
    CHECK(near(r.lx, 0) && near(r.ly, 0) && near(r.rx, 0) && near(r.ry, 0));
    CHECK(near(r.lt, 0) && near(r.rt, 0));

    /* sticks: +Y is up in both XInput and WGI */
    r = read_pad(0, 32767, 32767, -32768, -32768, 255, 0);
    CHECK(near(r.lx, 1) && near(r.ly, 1));
    CHECK(near(r.rx, -1) && near(r.ry, -1));
    CHECK(near(r.lt, 1) && near(r.rt, 0));
    r = read_pad(0, 16384, -16384, 0, 0, 0, 128);
    CHECK(near(r.lx, (16384 + 32768) * 2. / 65535 - 1));
    CHECK(r.ly < -0.49 && r.ly > -0.51);
    CHECK(near(r.rt, 128 / 255.));

    /* buttons */
    r = read_pad(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_START, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(r.buttons, GB_A | GB_Y | GB_LS | GB_MENU);
    r = read_pad(XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_BACK |
                 XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(r.buttons, GB_B | GB_X | GB_RS | GB_VIEW | GB_LTHUMB | GB_RTHUMB);

    /* the D-pad through the switch, each direction and diagonal */
    CHECK_EQ(read_pad(XINPUT_GAMEPAD_DPAD_UP, 0, 0, 0, 0, 0, 0).buttons, GB_UP);
    CHECK_EQ(read_pad(XINPUT_GAMEPAD_DPAD_DOWN, 0, 0, 0, 0, 0, 0).buttons, GB_DOWN);
    CHECK_EQ(read_pad(XINPUT_GAMEPAD_DPAD_LEFT, 0, 0, 0, 0, 0, 0).buttons, GB_LEFT);
    CHECK_EQ(read_pad(XINPUT_GAMEPAD_DPAD_RIGHT, 0, 0, 0, 0, 0, 0).buttons, GB_RIGHT);
    CHECK_EQ(read_pad(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, 0, 0, 0, 0, 0, 0).buttons, GB_UP | GB_RIGHT);
    CHECK_EQ(read_pad(XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, 0, 0, 0, 0, 0, 0).buttons, GB_DOWN | GB_LEFT);
    CHECK_EQ(read_pad(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN, 0, 0, 0, 0, 0, 0).buttons, 0);
}

static void test_vibration(void)
{
    XINPUT_VIBRATION v;

    wgi_host_vibration(65535, 1000, &v);
    CHECK_EQ(v.wLeftMotorSpeed, 65535);
    CHECK_EQ(v.wRightMotorSpeed, 1000);
    CHECK_EQ(WGI_HOST_VENDOR, 0x045e);
    CHECK_EQ(WGI_HOST_PRODUCT, 0x028e);
}

int main(void)
{
    test_paths();
    test_state();
    test_vibration();
    return test_report("test_wgi_host");
}
