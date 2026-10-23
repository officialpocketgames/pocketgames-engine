// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 8e: Windows.Gaming.Input's view of an XInput host slot. provider.c
// (a modified copy of upstream's) makes a host-slot provider for each slot device it is told
// about; this header holds what such a provider answers, as plain functions over the XInput
// state, so the host test tests/test_wgi_host.c runs them. gamepad.c (upstream, unchanged)
// turns the provider's state into a GamepadReading: thumbsticks from axes 0-3 as 2 * a - 1
// (left Y, left X, right Y, right X), triggers from axes 4 and 5, buttons 0-9 (A B X Y LB RB
// View Menu LS RS), the D-pad from switch 0.
//
// Needs windef.h and xinput.h.

#ifndef __WINE_WGI_HOST_H
#define __WINE_WGI_HOST_H

#define WGI_HOST_SLOTS      4
#define WGI_HOST_VENDOR     0x045e
#define WGI_HOST_PRODUCT    0x028e
#define WGI_HOST_AXES       6
#define WGI_HOST_BUTTONS    10
#define WGI_HOST_SWITCHES   1

/* NtUserGetGamepadState operations (spec 12 Part F2) */
#define WGI_HOST_OP_STATE       0
#define WGI_HOST_OP_VIBRATION   2

/* GameControllerSwitchPosition: Center, then Up and clockwise */
#define WGI_HOST_SWITCH_CENTER  0

/* The slot a device path names, or -1: the slot devices' paths (spec 15 clarification 8a),
 * compared without regard to case as Windows paths are. */
static inline int wgi_host_slot_from_path( const WCHAR *path )
{
    static const char prefix[] = "\\\\?\\HID#VID_045E&PID_028E&IG_0";
    static const char middle[] = "#9&4d616465&1&000";
    static const char suffix[] = "#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    const char *parts[3] = { prefix, middle, suffix };
    int slot = -1, part, i;

    if (!path) return -1;
    for (part = 0; part < 3; part++)
    {
        for (i = 0; parts[part][i]; i++, path++)
        {
            WCHAR a = *path, b = (WCHAR)(unsigned char)parts[part][i];
            if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
            if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
            if (a != b) return -1;
        }
        if (part == 2) break;
        if (*path < '0' || *path > '3') return -1;
        if (part == 0) slot = *path - '0';
        else if (*path - '0' != slot) return -1;
        path++;
    }
    return *path ? -1 : slot;
}

/* The provider's state from the slot's XInput state: axes 0-5, buttons 0-9, switch 0. */
static inline void wgi_host_map( const XINPUT_GAMEPAD *pad, double axes[WGI_HOST_AXES],
                                 unsigned char buttons[WGI_HOST_BUTTONS], int *switch_position )
{
    static const WORD masks[WGI_HOST_BUTTONS] =
    {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    };
    /* [vertical + 1][horizontal + 1], vertical +1 = up, horizontal +1 = right */
    static const int positions[3][3] = { { 6, 5, 4 }, { 7, 0, 3 }, { 8, 1, 2 } };
    int v, h, i;

    axes[0] = (pad->sThumbLY + 32768) / 65535.;
    axes[1] = (pad->sThumbLX + 32768) / 65535.;
    axes[2] = (pad->sThumbRY + 32768) / 65535.;
    axes[3] = (pad->sThumbRX + 32768) / 65535.;
    axes[4] = pad->bLeftTrigger / 255.;
    axes[5] = pad->bRightTrigger / 255.;
    for (i = 0; i < WGI_HOST_BUTTONS; i++) buttons[i] = (pad->wButtons & masks[i]) != 0;

    v = !!(pad->wButtons & XINPUT_GAMEPAD_DPAD_UP) - !!(pad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    h = !!(pad->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) - !!(pad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    *switch_position = positions[v + 1][h + 1];
}

/* put_Vibration: the rumble (left, heavy) and buzz (right, light) motors as XInput's. */
static inline void wgi_host_vibration( unsigned short rumble, unsigned short buzz, XINPUT_VIBRATION *out )
{
    out->wLeftMotorSpeed = rumble;
    out->wRightMotorSpeed = buzz;
}

#endif /* __WINE_WGI_HOST_H */
