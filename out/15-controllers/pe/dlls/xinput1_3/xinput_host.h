// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part D: XInput's host controller slots, answered by win32u
// (NtUserCallTwoParam_GetGamepadState, spec 12 Part F). Included by main.c, which defines
//   XINPUT_HOST_CALL(index, op, buffer)   NtUserGetGamepadState(index, op, buffer)
//   XINPUT_HOST_LOCK() / XINPUT_HOST_UNLOCK()   its host_crit critical section
// and by the host test tests/test_xinput_sdl.c, which defines them over a fake win32u.
// A win32u that doesn't know the call answers 0 ("not connected"), and main.c's HID path then
// runs as upstream.
//
// Needs windef.h, winerror.h, xinput.h and xinput_keystroke.h.

#ifndef __WINE_XINPUT_HOST_H
#define __WINE_XINPUT_HOST_H

#define HOST_PAD_STATE      0   /* XINPUT_STATE out */
#define HOST_PAD_CAPS       1   /* XINPUT_CAPABILITIES out */
#define HOST_PAD_VIBRATION  2   /* XINPUT_VIBRATION in */

/* The identity XInputGetCapabilitiesEx gives a host slot: an Xbox 360 controller, so readers
 * pick Xbox glyphs (QUESTIONS 15-15). */
#define HOST_PAD_VENDOR     0x045e
#define HOST_PAD_PRODUCT    0x028e
#define HOST_PAD_VERSION    0x0114

static BOOL host_disabled;                                  /* XInputEnable(FALSE) */
static XINPUT_VIBRATION host_vibration[XUSER_MAX_COUNT];    /* the game's last XInputSetState */
static BOOL host_motors_on[XUSER_MAX_COUNT];                /* what was last sent was not 0, 0 */
static XINPUT_GAMEPAD host_last_keystroke[XUSER_MAX_COUNT]; /* XInputGetKeystroke's edge memory */

static BOOL host_connected(DWORD index)
{
    XINPUT_STATE state;
    return index < XUSER_MAX_COUNT && XINPUT_HOST_CALL(index, HOST_PAD_STATE, &state);
}

/* One call, no lock, no allocation: games poll this at up to 1 kHz. */
static BOOL host_get_state(DWORD index, XINPUT_STATE *state)
{
    if (index >= XUSER_MAX_COUNT) return FALSE;
    if (!XINPUT_HOST_CALL(index, HOST_PAD_STATE, state)) return FALSE;
    if (host_disabled) memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    return TRUE;
}

/* Called with the lock held (or at process detach). */
static void host_send_vibration(DWORD index, const XINPUT_VIBRATION *vibration)
{
    XINPUT_VIBRATION tmp = *vibration;
    XINPUT_HOST_CALL(index, HOST_PAD_VIBRATION, &tmp);
    host_motors_on[index] = vibration->wLeftMotorSpeed || vibration->wRightMotorSpeed;
}

static BOOL host_set_state(DWORD index, const XINPUT_VIBRATION *vibration)
{
    if (!vibration || !host_connected(index)) return FALSE;

    XINPUT_HOST_LOCK();
    host_vibration[index] = *vibration;
    if (!host_disabled) host_send_vibration(index, vibration);
    XINPUT_HOST_UNLOCK();
    return TRUE;
}

/* XInputEnable for the host slots: FALSE silences the motors (and host_get_state zeroes the
 * reported state); TRUE sends the stored motors again, as Windows does. */
static void host_enable(BOOL enable)
{
    static const XINPUT_VIBRATION off;
    DWORD index;

    XINPUT_HOST_LOCK();
    host_disabled = !enable;
    for (index = 0; index < XUSER_MAX_COUNT; index++)
    {
        if (!host_connected(index)) continue;
        if (!enable && host_motors_on[index]) host_send_vibration(index, &off);
        else if (enable && (host_vibration[index].wLeftMotorSpeed || host_vibration[index].wRightMotorSpeed))
            host_send_vibration(index, &host_vibration[index]);
    }
    XINPUT_HOST_UNLOCK();
}

/* Process detach: the session outlives the game, so stop the motors it left running. No lock:
 * at process exit the other threads are gone and may have held it. */
static void host_stop_motors(void)
{
    static const XINPUT_VIBRATION off;
    DWORD index;

    for (index = 0; index < XUSER_MAX_COUNT; index++)
        if (host_motors_on[index]) host_send_vibration(index, &off);
}

/* XInputGetKeystroke for a host slot. Returns FALSE (and forgets the slot's edges) when the
 * slot isn't connected, so the HID path can answer. */
static BOOL host_get_keystroke(DWORD index, XINPUT_KEYSTROKE *keystroke, DWORD *ret)
{
    XINPUT_STATE state;

    if (!host_get_state(index, &state))
    {
        if (index < XUSER_MAX_COUNT)
        {
            XINPUT_HOST_LOCK();
            memset(&host_last_keystroke[index], 0, sizeof(host_last_keystroke[index]));
            XINPUT_HOST_UNLOCK();
        }
        return FALSE;
    }

    XINPUT_HOST_LOCK();
    *ret = xinput_keystroke_edge(index, &state.Gamepad, &host_last_keystroke[index], keystroke);
    XINPUT_HOST_UNLOCK();
    return TRUE;
}

/* XInputGetCapabilities(Ex) for a connected host slot: win32u's Caps operation, plus the
 * identity above for the Ex form. Returns FALSE when the slot isn't connected. */
static BOOL host_get_capabilities_ex(DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps, DWORD *ret)
{
    XINPUT_CAPABILITIES host_caps;

    if (index >= XUSER_MAX_COUNT || !caps) return FALSE;
    if (!XINPUT_HOST_CALL(index, HOST_PAD_CAPS, &host_caps)) return FALSE;

    if (flags & XINPUT_FLAG_GAMEPAD && host_caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD)
    {
        *ret = ERROR_DEVICE_NOT_CONNECTED;
        return TRUE;
    }
    memset(caps, 0, sizeof(*caps));
    caps->Capabilities = host_caps;
    caps->VendorId = HOST_PAD_VENDOR;
    caps->ProductId = HOST_PAD_PRODUCT;
    caps->VersionNumber = HOST_PAD_VERSION;
    *ret = ERROR_SUCCESS;
    return TRUE;
}

/* XInputGetBatteryInformation for a connected host slot (clarification 2): the store has no
 * battery for an XInput slot, so the pad's is unknown; there is no headset. */
static BOOL host_get_battery(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION *battery, DWORD *ret)
{
    if (!host_connected(index)) return FALSE;
    if (!battery)
    {
        *ret = ERROR_BAD_ARGUMENTS;
        return TRUE;
    }
    if (type == BATTERY_DEVTYPE_GAMEPAD)
    {
        battery->BatteryType = BATTERY_TYPE_UNKNOWN;
        battery->BatteryLevel = BATTERY_LEVEL_EMPTY;
    }
    else
    {
        battery->BatteryType = BATTERY_TYPE_DISCONNECTED;
        battery->BatteryLevel = BATTERY_LEVEL_EMPTY;
    }
    *ret = ERROR_SUCCESS;
    return TRUE;
}

/* XInputGetDSoundAudioDeviceGuids for a connected host slot: no headset, GUID_NULL. */
static BOOL host_get_dsound_guids(DWORD index, GUID *render_guid, GUID *capture_guid)
{
    if (!host_connected(index)) return FALSE;
    memset(render_guid, 0, sizeof(*render_guid));
    memset(capture_guid, 0, sizeof(*capture_guid));
    return TRUE;
}

#endif /* __WINE_XINPUT_HOST_H */
