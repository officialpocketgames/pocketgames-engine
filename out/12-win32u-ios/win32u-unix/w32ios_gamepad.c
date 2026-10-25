// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part F: XInput through win32u. NtUserCallTwoParam_GetGamepadState reads the app's
// controller snapshot (contract Section B/C: pgios_gamepad_get_state, set_vibration,
// rumble_caps) without allocating or asking the server. arg1 = user index (bits 0-7) | operation
// (bits 8-15: 0 State, 1 Caps, 2 SetVibration); arg2 = the caller's XINPUT_STATE /
// XINPUT_CAPABILITIES / XINPUT_VIBRATION (a host address: WoW64's thunk converts it). The
// packing is w32ios_gamepad_answer (w32ios_pure.c).

#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/debug.h"
#include "w32ios.h"
#include "w32ios_app.h"

static int get_state( int index, void *pad )
{
    return pgios_gamepad_get_state( index, pad );
}

static const struct w32ios_pad_source source =
{
    get_state,
    pgios_gamepad_set_vibration,
    pgios_gamepad_rumble_caps,
};

ULONG_PTR w32ios_gamepad_call( ULONG_PTR arg1, ULONG_PTR arg2 )
{
    UINT index = arg1 & 0xff, op = (arg1 >> 8) & 0xff;

    /* E6: controller input goes to the foreground window */
    if (!index && op == W32IOS_PAD_STATE && arg2) w32ios_foreground_fix();
    return w32ios_gamepad_answer( &source, (UINT)arg1, (void *)arg2 );
}
