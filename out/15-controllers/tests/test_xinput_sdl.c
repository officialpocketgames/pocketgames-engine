// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 7 (device report of build 36, the Xbox look): what an SDL2 game's
// XInput driver (SDL 2.0.10-2.30) asks xinput1_4 when it looks for controllers and reads one,
// run against the host-slot answers of pe/dlls/xinput1_3 (xinput_host.h, the same file main.c
// includes) over a fake win32u that answers spec 12 Part F2's three operations from a fake store.
// The sequence is written from general knowledge of SDL2's XInput driver, not from its source:
//   detect:  XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD) for i = 3..0; for each success,
//            XInputGetCapabilitiesEx(1, i, 0) (ordinal 108) for the VID/PID/version, the
//            SubType for the controller type;
//   open:    XInputGetCapabilities again, XInputGetBatteryInformation(gamepad);
//   update:  XInputGetStateEx (ordinal 100) every frame, input taken when dwPacketNumber
//            changes; XInputSetState for rumble; ERROR_DEVICE_NOT_CONNECTED drops the pad.
// The entry points below are main.c's, cut to their host-slot part plus "no HID pad"
// (ERROR_DEVICE_NOT_CONNECTED), which is what a session without XInput HID devices answers.
// Needs Wine 11.4's include/ for windef.h, winerror.h and xinput.h.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winerror.h"
#include "xinput.h"

#include "test_common.h"

#define XINPUT_GAMEPAD_GUIDE 0x0400   /* as main.c defines it */

/* ---- the fake win32u: NtUserGetGamepadState(index, op, buffer) (spec 12 Part F2) ---------- */

static struct
{
    BOOL connected;
    XINPUT_STATE state;
    XINPUT_VIBRATION vibration;
    unsigned int vibration_calls;
} store[XUSER_MAX_COUNT];
static unsigned int host_calls, lock_depth, max_lock_depth;

static BOOL fake_get_gamepad_state(UINT index, UINT op, void *buffer)
{
    host_calls++;
    if (index >= XUSER_MAX_COUNT || !store[index].connected) return FALSE;
    switch (op)
    {
    case 0:
        *(XINPUT_STATE *)buffer = store[index].state;
        return TRUE;
    case 1:
    {
        XINPUT_CAPABILITIES *caps = buffer;
        memset(caps, 0, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Flags = 0;
        caps->Gamepad.wButtons = 0xf3ff;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xff;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = (SHORT)0xffc0;
        caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xffc0;
        caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xff;
        return TRUE;
    }
    case 2:
        store[index].vibration = *(XINPUT_VIBRATION *)buffer;
        store[index].vibration_calls++;
        return TRUE;
    }
    return FALSE;
}

#define XINPUT_HOST_CALL(index, op, buffer) fake_get_gamepad_state(index, op, buffer)
#define XINPUT_HOST_LOCK() do { if (++lock_depth > max_lock_depth) max_lock_depth = lock_depth; } while (0)
#define XINPUT_HOST_UNLOCK() do { lock_depth--; } while (0)

#include "xinput_keystroke.h"
#include "xinput_host.h"

/* ---- main.c's entry points, host part (no HID pads in this session) ---------------------- */

static DWORD WINAPI test_XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    DWORD ret = ERROR_SUCCESS;
    if (host_get_capabilities_ex(index, flags, caps, &ret)) return ret;
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    return ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD WINAPI test_XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities)
{
    XINPUT_CAPABILITIES_EX caps_ex;
    DWORD ret = test_XInputGetCapabilitiesEx(1, index, flags, &caps_ex);
    if (!ret) *capabilities = caps_ex.Capabilities;
    return ret;
}

static DWORD WINAPI test_XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;
    if (host_get_state(index, state)) return ERROR_SUCCESS;
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    return ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD WINAPI test_XInputGetState(DWORD index, XINPUT_STATE *state)
{
    DWORD ret = test_XInputGetStateEx(index, state);
    if (ret != ERROR_SUCCESS) return ret;
    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;
    return ERROR_SUCCESS;
}

static DWORD WINAPI test_XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    if (host_set_state(index, vibration)) return ERROR_SUCCESS;
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    return ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD WINAPI test_XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION *battery)
{
    DWORD ret;
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (host_get_battery(index, type, battery, &ret)) return ret;
    return ERROR_DEVICE_NOT_CONNECTED;
}

/* ---- an SDL2-style XInput driver ---------------------------------------------------------- */

struct sdl_pad
{
    BOOL attached;
    WORD vendor, product, version;
    BYTE subtype;
    DWORD packet;
    WORD buttons;
    SHORT left_x;
    BYTE battery_type;
};

static struct sdl_pad sdl[XUSER_MAX_COUNT];

/* detection: returns how many pads it has; drops pads that went away */
static int sdl_detect(void)
{
    XINPUT_CAPABILITIES caps;
    XINPUT_CAPABILITIES_EX caps_ex;
    int i, count = 0;

    for (i = XUSER_MAX_COUNT - 1; i >= 0; i--)
    {
        if (test_XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS)
        {
            sdl[i].attached = FALSE;
            continue;
        }
        if (!sdl[i].attached)
        {
            memset(&sdl[i], 0, sizeof(sdl[i]));
            sdl[i].attached = TRUE;
            sdl[i].subtype = caps.SubType;
            if (test_XInputGetCapabilitiesEx(1, i, 0, &caps_ex) == ERROR_SUCCESS)
            {
                sdl[i].vendor = caps_ex.VendorId;
                sdl[i].product = caps_ex.ProductId;
                sdl[i].version = caps_ex.VersionNumber;
            }
        }
        count++;
    }
    return count;
}

static void sdl_open(int i)
{
    XINPUT_CAPABILITIES caps;
    XINPUT_BATTERY_INFORMATION battery;

    CHECK_EQ(test_XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD, &caps), ERROR_SUCCESS);
    memset(&battery, 0xcc, sizeof(battery));
    CHECK_EQ(test_XInputGetBatteryInformation(i, BATTERY_DEVTYPE_GAMEPAD, &battery), ERROR_SUCCESS);
    sdl[i].battery_type = battery.BatteryType;
}

/* one frame; returns 0 when the pad is gone */
static int sdl_update(int i)
{
    XINPUT_STATE state;

    if (test_XInputGetStateEx(i, &state) != ERROR_SUCCESS) return 0;
    if (state.dwPacketNumber && state.dwPacketNumber != sdl[i].packet)
    {
        sdl[i].packet = state.dwPacketNumber;
        sdl[i].buttons = state.Gamepad.wButtons;
        sdl[i].left_x = state.Gamepad.sThumbLX;
    }
    return 1;
}

static void test_detect_slot0(void)
{
    XINPUT_VIBRATION rumble = { 0x8000, 0x4000 };

    memset(store, 0, sizeof(store));
    memset(sdl, 0, sizeof(sdl));
    CHECK_EQ(sdl_detect(), 0);                       /* nothing published yet */

    store[0].connected = TRUE;
    store[0].state.dwPacketNumber = 1;
    CHECK_EQ(sdl_detect(), 1);
    CHECK_EQ(sdl[0].attached, TRUE);
    CHECK_EQ(sdl[1].attached, FALSE);
    CHECK_EQ(sdl[0].subtype, XINPUT_DEVSUBTYPE_GAMEPAD);
    CHECK_EQ(sdl[0].vendor, 0x045e);                 /* an Xbox 360 controller: Xbox glyphs */
    CHECK_EQ(sdl[0].product, 0x028e);
    CHECK_EQ(sdl[0].version, 0x0114);

    sdl_open(0);
    CHECK_EQ(sdl[0].battery_type, BATTERY_TYPE_UNKNOWN);

    /* input: A and the left stick, then the guide button through the Ex call only */
    store[0].state.dwPacketNumber = 2;
    store[0].state.Gamepad.wButtons = XINPUT_GAMEPAD_A;
    store[0].state.Gamepad.sThumbLX = 12345;
    CHECK_EQ(sdl_update(0), 1);
    CHECK_EQ(sdl[0].buttons, XINPUT_GAMEPAD_A);
    CHECK_EQ(sdl[0].left_x, 12345);
    store[0].state.dwPacketNumber = 3;
    store[0].state.Gamepad.wButtons = XINPUT_GAMEPAD_GUIDE;
    CHECK_EQ(sdl_update(0), 1);
    CHECK_EQ(sdl[0].buttons, XINPUT_GAMEPAD_GUIDE);
    {
        XINPUT_STATE plain;
        CHECK_EQ(test_XInputGetState(0, &plain), ERROR_SUCCESS);
        CHECK_EQ(plain.Gamepad.wButtons, 0);
    }
    /* the same packet number: nothing new */
    store[0].state.Gamepad.wButtons = XINPUT_GAMEPAD_B;
    CHECK_EQ(sdl_update(0), 1);
    CHECK_EQ(sdl[0].buttons, XINPUT_GAMEPAD_GUIDE);

    /* rumble reaches the store, under the lock, without nesting */
    CHECK_EQ(test_XInputSetState(0, &rumble), ERROR_SUCCESS);
    CHECK_EQ(store[0].vibration_calls, 1);
    CHECK_EQ(store[0].vibration.wLeftMotorSpeed, 0x8000);
    CHECK_EQ(store[0].vibration.wRightMotorSpeed, 0x4000);
    CHECK_EQ(lock_depth, 0);
    CHECK_EQ(max_lock_depth, 1);

    /* a second pad on slot 2 is found by the next detection, slot 0 kept */
    store[2].connected = TRUE;
    store[2].state.dwPacketNumber = 7;
    CHECK_EQ(sdl_detect(), 2);
    CHECK_EQ(sdl[2].attached, TRUE);
    CHECK_EQ(sdl[0].packet, 3);                      /* not re-created */

    /* slot 0 unplugged: the update fails, the next detection drops it */
    store[0].connected = FALSE;
    CHECK_EQ(sdl_update(0), 0);
    CHECK_EQ(test_XInputSetState(0, &rumble), ERROR_DEVICE_NOT_CONNECTED);
    CHECK_EQ(sdl_detect(), 1);
    CHECK_EQ(sdl[0].attached, FALSE);
    CHECK_EQ(sdl[2].attached, TRUE);
}

/* The answers SDL's checks depend on, slot by slot. */
static void test_answers(void)
{
    XINPUT_CAPABILITIES_EX caps_ex;
    XINPUT_BATTERY_INFORMATION battery;
    GUID render, capture;
    XINPUT_STATE state;

    memset(store, 0, sizeof(store));
    store[1].connected = TRUE;

    CHECK_EQ(test_XInputGetCapabilitiesEx(1, 0, 0, &caps_ex), ERROR_DEVICE_NOT_CONNECTED);
    CHECK_EQ(test_XInputGetCapabilitiesEx(1, 4, 0, &caps_ex), ERROR_BAD_ARGUMENTS);
    memset(&caps_ex, 0xcc, sizeof(caps_ex));
    CHECK_EQ(test_XInputGetCapabilitiesEx(1, 1, XINPUT_FLAG_GAMEPAD, &caps_ex), ERROR_SUCCESS);
    CHECK_EQ(caps_ex.Capabilities.Type, XINPUT_DEVTYPE_GAMEPAD);
    CHECK_EQ(caps_ex.Capabilities.SubType, XINPUT_DEVSUBTYPE_GAMEPAD);
    CHECK_EQ(caps_ex.VendorId, 0x045e);
    CHECK_EQ(caps_ex.ProductId, 0x028e);
    CHECK_EQ(caps_ex.VersionNumber, 0x0114);
    CHECK_EQ(caps_ex.unk1, 0);                       /* zeroed, not left as garbage */
    CHECK_EQ(caps_ex.unk2, 0);

    CHECK_EQ(test_XInputGetStateEx(0, &state), ERROR_DEVICE_NOT_CONNECTED);
    CHECK_EQ(test_XInputGetStateEx(1, NULL), ERROR_BAD_ARGUMENTS);
    CHECK_EQ(test_XInputGetBatteryInformation(1, BATTERY_DEVTYPE_HEADSET, &battery), ERROR_SUCCESS);
    CHECK_EQ(battery.BatteryType, BATTERY_TYPE_DISCONNECTED);
    CHECK_EQ(test_XInputGetBatteryInformation(1, BATTERY_DEVTYPE_GAMEPAD, &battery), ERROR_SUCCESS);
    CHECK_EQ(battery.BatteryType, BATTERY_TYPE_UNKNOWN);
    CHECK_EQ(battery.BatteryLevel, BATTERY_LEVEL_EMPTY);
    CHECK_EQ(host_get_dsound_guids(1, &render, &capture), TRUE);
    CHECK_EQ(render.Data1, 0);
    CHECK_EQ(host_get_dsound_guids(0, &render, &capture), FALSE);

    /* XInputEnable(FALSE): state reads as at rest, motors stop; TRUE restores them */
    {
        XINPUT_VIBRATION rumble = { 100, 200 };
        store[1].state.Gamepad.wButtons = XINPUT_GAMEPAD_X;
        CHECK_EQ(test_XInputSetState(1, &rumble), ERROR_SUCCESS);
        host_enable(FALSE);
        CHECK_EQ(store[1].vibration.wLeftMotorSpeed, 0);
        CHECK_EQ(test_XInputGetStateEx(1, &state), ERROR_SUCCESS);
        CHECK_EQ(state.Gamepad.wButtons, 0);
        host_enable(TRUE);
        CHECK_EQ(store[1].vibration.wLeftMotorSpeed, 100);
        CHECK_EQ(store[1].vibration.wRightMotorSpeed, 200);
        CHECK_EQ(test_XInputGetStateEx(1, &state), ERROR_SUCCESS);
        CHECK_EQ(state.Gamepad.wButtons, XINPUT_GAMEPAD_X);

        /* process detach stops the motors the game left running */
        host_stop_motors();
        CHECK_EQ(store[1].vibration.wLeftMotorSpeed, 0);
        CHECK_EQ(store[1].vibration.wRightMotorSpeed, 0);
    }

    /* XInputGetKeystroke: a connected slot answers from its edges, an empty one falls through */
    {
        XINPUT_KEYSTROKE ks;
        DWORD ret = 0xdead;

        CHECK_EQ(host_get_keystroke(0, &ks, &ret), FALSE);
        CHECK_EQ(ret, 0xdead);
        CHECK_EQ(host_get_keystroke(1, &ks, &ret), TRUE);
        CHECK_EQ(ret, ERROR_SUCCESS);
        CHECK_EQ(ks.VirtualKey, VK_PAD_X);
        CHECK_EQ(ks.Flags, XINPUT_KEYSTROKE_KEYDOWN);
        CHECK_EQ(ks.UserIndex, 1);
        CHECK_EQ(host_get_keystroke(1, &ks, &ret), TRUE);
        CHECK_EQ(ret, ERROR_EMPTY);
    }
}

int main(void)
{
    test_detect_slot0();
    test_answers();
    CHECK(host_calls > 0);
    return test_report("test_xinput_sdl");
}
