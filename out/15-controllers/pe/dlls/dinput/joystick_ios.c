// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part E: an opt-in DirectInput joystick backed by host controller slot 0.
//
// Without winebus.sys, IDirectInput8::EnumDevices(DI8DEVCLASS_GAMECTRL) finds nothing, so a
// game that reads only DirectInput sees no controller. With MADEIRA_DINPUT_PAD starting with
// '1', this device offers slot 0 (read through win32u, NtUserGetGamepadState, spec 12 Part F)
// as one standard gamepad. It plugs into dinput's generic device code (device.c) the way the
// HID joystick does, and scales its axes with the HID joystick's own functions
// (joystick_scale.h), so DIPROP_RANGE, DIPROP_DEADZONE and DIPROP_SATURATION behave the same.
// No force feedback.

#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winuser.h"
#include "winerror.h"
#include "winreg.h"

#include "dinput.h"
#include "xinput.h"
#include "ntuser.h"

#include "dinput_private.h"
#include "device_private.h"
#include "joystick_scale.h"
#include "joystick_ios_map.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

#define HOST_PAD_STATE      0           /* NtUserGetGamepadState operation: XINPUT_STATE */
#define IOS_JOYSTICK_VID    0x1209      /* pid.codes; not 045E, which some games skip */
#define IOS_JOYSTICK_PID    0x4d47
#define IOS_JOYSTICK_NAME   L"MYIOSDECK Gamepad"

/* E2: fixed, so a game that stored it re-creates the device from it next run. */
static const GUID ios_joystick_guid =
    { 0x6d796f73, 0x6465, 0x636b, { 0x64, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x00, 0x01 } };

struct ios_joystick
{
    struct dinput_device base;
    DWORD trace_time;               /* E6: GetTickCount of the last trace */
    BOOL traced;
};

static inline struct ios_joystick *impl_from_IDirectInputDevice8W( IDirectInputDevice8W *iface )
{
    return CONTAINING_RECORD( CONTAINING_RECORD( iface, struct dinput_device, IDirectInputDevice8W_iface ),
                              struct ios_joystick, base );
}

/* ---- E1: opt-in ------------------------------------------------------------------------ */

static BOOL env_starts_with_1( const WCHAR *name, char *copy, DWORD copy_size )
{
    WCHAR value[256];
    DWORD len = GetEnvironmentVariableW( name, value, ARRAY_SIZE(value) ), i;

    if (!len || len >= ARRAY_SIZE(value))
    {
        lstrcpynA( copy, len ? "(too long)" : "(unset)", copy_size );
        return FALSE;
    }
    for (i = 0; i < len && i + 1 < copy_size; i++) copy[i] = value[i] < 0x80 ? (char)value[i] : '?';
    copy[i] = 0;
    return value[0] == '1';
}

/* MADEIRA_DINPUT_PAD, read once and logged once. */
static BOOL ios_joystick_enabled(void)
{
    static LONG state = -1;     /* -1 not read yet, 0 off, 1 on */
    char copy[64];
    LONG enabled;

    if (state >= 0) return state;
    enabled = env_starts_with_1( L"MADEIRA_DINPUT_PAD", copy, sizeof(copy) );
    if (InterlockedCompareExchange( &state, enabled, -1 ) == -1)
        MESSAGE( "[dinput-ios] MADEIRA_DINPUT_PAD=%s: the DirectInput joystick for host slot 0 is %s\n",
                 copy, enabled ? "on" : "off" );
    return state;
}

/* MADEIRA_DINPUT_TRACE, read once. */
static BOOL ios_joystick_trace(void)
{
    static LONG state = -1;
    char copy[64];

    if (state < 0) InterlockedCompareExchange( &state, env_starts_with_1( L"MADEIRA_DINPUT_TRACE", copy, sizeof(copy) ), -1 );
    return state;
}

static BOOL host_slot0( XINPUT_STATE *state )
{
    return NtUserGetGamepadState( 0, HOST_PAD_STATE, state );
}

/* ---- E2: identity ----------------------------------------------------------------------- */

static GUID ios_joystick_product(void)
{
    GUID product = dinput_pidvid_guid;
    product.Data1 = MAKELONG( IOS_JOYSTICK_VID, IOS_JOYSTICK_PID );
    return product;
}

static void fill_instance( DIDEVICEINSTANCEW *instance, DWORD version )
{
    instance->guidInstance = ios_joystick_guid;
    instance->guidProduct = ios_joystick_product();
    if (version >= 0x0800)
        instance->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
    else
        instance->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8) | DIDEVTYPE_HID;
    lstrcpynW( instance->tszInstanceName, IOS_JOYSTICK_NAME, MAX_PATH );
    lstrcpynW( instance->tszProductName, IOS_JOYSTICK_NAME, MAX_PATH );
    instance->guidFFDriver = GUID_NULL;     /* no force feedback: the FF filter skips it */
    instance->wUsagePage = 0x01;            /* Generic Desktop */
    instance->wUsage = 0x05;                /* Game Pad */
}

/* Spec 15 clarification 8: slot 0's own HID device (the XInput HID collection), which dinput's
 * HID joystick lists when it opens. While it does, this joystick would be a second one for the
 * same pad, so it steps aside (logged once each way). */
static BOOL slot0_device_listed(void)
{
    static const WCHAR path[] =
        L"\\\\?\\HID#VID_045E&PID_028E&IG_00#9&4d616465&1&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    static LONG logged = -1;
    HANDLE file;
    LONG listed;

    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if ((listed = file != INVALID_HANDLE_VALUE)) CloseHandle( file );
    if (InterlockedExchange( &logged, listed ) != listed)
        MESSAGE( "[dinput-ios] XInput slot 0's HID device is %s: the iOS joystick %s\n",
                 listed ? "listed" : "not listed", listed ? "steps aside" : "is offered" );
    return listed;
}

/* Checked on every enumeration and every CreateDevice, never cached: disabled, no pad in
 * slot 0, slot 0's own HID device listed, or on Wine's DirectInput joystick disable list reads
 * as a PC with nothing plugged in. */
HRESULT ios_joystick_enum_device( DWORD type, DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version )
{
    XINPUT_STATE state;
    BOOL override;

    if (!ios_joystick_enabled()) return DIERR_DEVICENOTREG;
    if (!host_slot0( &state )) return DIERR_DEVICENOTREG;
    if (slot0_device_listed()) return DIERR_DEVICENOTREG;

    fill_instance( instance, version );
    if (device_instance_is_disabled( instance, &override )) return DIERR_DEVICENOTREG;

    TRACE( "found the iOS joystick, type %#lx, product %s, instance %s\n", instance->dwDevType,
           debugstr_guid( &instance->guidProduct ), debugstr_guid( &instance->guidInstance ) );
    return DI_OK;
}

/* ---- E3: objects ------------------------------------------------------------------------- */

static void make_object( const struct ios_joy_object *object, DIDEVICEOBJECTINSTANCEW *instance )
{
    static const GUID *const axis_guids[IOS_JOY_AXES] =
    {
        &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis,
    };
    const char *name = object->name;
    int i;

    memset( instance, 0, sizeof(*instance) );
    instance->dwSize = sizeof(*instance);
    switch (object->kind)
    {
    case IOS_JOY_AXIS:
        instance->guidType = *axis_guids[object->instance];
        instance->dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( object->instance );
        instance->dwFlags = DIDOI_ASPECTPOSITION;
        break;
    case IOS_JOY_POV:
        instance->guidType = GUID_POV;
        instance->dwType = DIDFT_POV | DIDFT_MAKEINSTANCE( object->instance );
        break;
    case IOS_JOY_BUTTON:
        instance->guidType = GUID_Button;
        instance->dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( object->instance );
        break;
    }
    instance->dwOfs = object->offset;
    instance->wUsagePage = object->usage_page;
    instance->wUsage = object->usage;
    for (i = 0; name[i] && i < MAX_PATH - 1; i++) instance->tszName[i] = name[i];
    instance->tszName[i] = 0;
}

static BOOL enum_objects( struct ios_joystick *impl, const DIPROPHEADER *filter, DWORD flags,
                          enum_object_callback callback, void *data )
{
    DIDEVICEOBJECTINSTANCEW instance;
    BOOL ret;
    UINT i;

    for (i = 0; i < IOS_JOY_OBJECTS; i++)
    {
        make_object( &ios_joy_objects[i], &instance );
        if (flags != DIDFT_ALL && !(flags & DIDFT_GETTYPE( instance.dwType ))) continue;

        switch (filter->dwHow)
        {
        case DIPH_DEVICE:
            break;
        case DIPH_BYOFFSET:
            if (filter->dwObj != instance.dwOfs) continue;
            break;
        case DIPH_BYID:
            if ((filter->dwObj & 0x00ffffff) != (instance.dwType & 0x00ffffff)) continue;
            break;
        case DIPH_BYUSAGE:
            if (HIWORD( filter->dwObj ) != instance.wUsagePage) continue;
            if (LOWORD( filter->dwObj ) != instance.wUsage) continue;
            break;
        default:
            FIXME( "unimplemented filter dwHow %#lx dwObj %#lx\n", filter->dwHow, filter->dwObj );
            continue;
        }

        ret = callback( &impl->base, i, NULL, &instance, data );
        if (ret != DIENUM_CONTINUE) return ret;
    }

    return DIENUM_CONTINUE;
}

/* ---- E4: ranges ------------------------------------------------------------------------- */

static BOOL init_object_properties( struct dinput_device *device, UINT index, struct hid_value_caps *caps,
                                    const DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    struct object_properties *properties = device->object_properties + index;
    const struct ios_joy_object *object = &ios_joy_objects[index];

    if (object->kind == IOS_JOY_AXIS)
    {
        properties->bit_size = IOS_JOY_AXIS_BITS;
        properties->logical_min = IOS_JOY_AXIS_LOGICAL_MIN;
        properties->logical_max = IOS_JOY_AXIS_LOGICAL_MAX;
        properties->physical_min = IOS_JOY_AXIS_LOGICAL_MIN;
        properties->physical_max = IOS_JOY_AXIS_LOGICAL_MAX;
        properties->range_min = 0;
        properties->range_max = IOS_JOY_AXIS_RANGE_MAX;
    }
    else
    {
        properties->bit_size = IOS_JOY_POV_BITS;
        properties->logical_min = 0;
        properties->logical_max = IOS_JOY_POV_LOGICAL_MAX;
        properties->physical_min = 0;
        properties->physical_max = IOS_JOY_POV_RANGE_MAX;
        properties->range_min = 0;
        properties->range_max = IOS_JOY_POV_RANGE_MAX;
    }
    properties->saturation = 10000;
    properties->granularity = 1;
    return DIENUM_CONTINUE;
}

/* ---- E6: trace -------------------------------------------------------------------------- */

static void trace_state( struct ios_joystick *impl, const XINPUT_STATE *state )
{
    const DIDATAFORMAT *format = &impl->base.user_format;
    const BYTE *s = impl->base.device_state;
    char buttons[IOS_JOY_BUTTONS + 1];
    DWORD now = GetTickCount(), i;

    if (impl->traced && now - impl->trace_time < 1000) return;
    impl->traced = TRUE;
    impl->trace_time = now;

    for (i = 0; i < IOS_JOY_BUTTONS; i++) buttons[i] = s[28 + i] ? '1' : '0';
    buttons[IOS_JOY_BUTTONS] = 0;
    MESSAGE( "[dinput-ios] state X %ld Y %ld Z %ld Rx %ld Ry %ld Rz %ld POV %ld buttons %s\n",
             *(const LONG *)(s + 0), *(const LONG *)(s + 4), *(const LONG *)(s + 8), *(const LONG *)(s + 12),
             *(const LONG *)(s + 16), *(const LONG *)(s + 20), *(const LONG *)(s + 24), buttons );
    MESSAGE( "[dinput-ios] raw packet %lu buttons %#x lt %u rt %u lx %d ly %d rx %d ry %d\n",
             state->dwPacketNumber, state->Gamepad.wButtons,
             state->Gamepad.bLeftTrigger, state->Gamepad.bRightTrigger, state->Gamepad.sThumbLX,
             state->Gamepad.sThumbLY, state->Gamepad.sThumbRX, state->Gamepad.sThumbRY );
    for (i = 0; i < IOS_JOY_AXES + 1; i++)
    {
        const struct object_properties *p = impl->base.object_properties + i;
        MESSAGE( "[dinput-ios] object %lu %s: range %ld..%ld deadzone %ld saturation %ld\n", i,
                 ios_joy_objects[i].name, p->range_min, p->range_max, p->deadzone, p->saturation );
    }
    if (format->rgodf)
    {
        for (i = 0; i < impl->base.device_format.dwNumObjs && i < format->dwNumObjs; i++)
            MESSAGE( "[dinput-ios] format: object %lu %s at game offset %lu, type %#lx\n", i,
                     ios_joy_objects[i].name, format->rgodf[i].dwOfs, format->rgodf[i].dwType );
    }
}

/* ---- the device ------------------------------------------------------------------------- */

/* Spec 15 clarification 3: slot 0 is empty. The device takes dinput's own "unplugged" state,
 * as a HID joystick that went away does: device.c then answers DIERR_INPUTLOST to Poll,
 * GetDeviceState and GetDeviceData until the game calls Acquire again. The stored state and
 * the queued events are left as they were. */
static HRESULT ios_joystick_lost( struct ios_joystick *impl )
{
    IDirectInputDevice8W *iface = &impl->base.IDirectInputDevice8W_iface;
    BOOL lost = FALSE;

    EnterCriticalSection( &impl->base.crit );
    if (impl->base.status == STATUS_ACQUIRED)
    {
        impl->base.status = STATUS_UNPLUGGED;
        lost = TRUE;
    }
    LeaveCriticalSection( &impl->base.crit );

    if (lost)
    {
        /* off the acquired list, as Unacquire does; Acquire puts it back */
        dinput_hooks_unacquire_device( iface );
        if (ios_joystick_trace()) MESSAGE( "[dinput-ios] slot 0 is empty: device lost until Acquire\n" );
    }
    return DIERR_INPUTLOST;
}

/* Poll (and GetDeviceState / GetDeviceData, which poll first): read slot 0, scale, store,
 * queue the changes. */
static HRESULT ios_joystick_poll( IDirectInputDevice8W *iface )
{
    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    BYTE old_state[DEVICE_STATE_MAX_SIZE];
    struct ios_joy_sample sample;
    XINPUT_STATE state;
    DWORD time = 0, seq = 0, i;
    BOOL changed = FALSE;
    LONG value, old;

    if (!host_slot0( &state )) return ios_joystick_lost( impl );
    ios_joy_from_gamepad( state.Gamepad.wButtons, state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger,
                          state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, state.Gamepad.sThumbRX,
                          state.Gamepad.sThumbRY, &sample );

    EnterCriticalSection( &impl->base.crit );
    memcpy( old_state, impl->base.device_state, impl->base.device_format.dwDataSize );
    for (i = 0; i < IOS_JOY_OBJECTS; i++)
    {
        const struct ios_joy_object *object = &ios_joy_objects[i];

        if (object->kind == IOS_JOY_BUTTON)
        {
            value = sample.value[i];
            old = old_state[object->offset];
            impl->base.device_state[object->offset] = (BYTE)value;
        }
        else
        {
            value = ios_joy_scaled_value( impl->base.object_properties + i, object, sample.value[i] );
            old = *(const LONG *)(old_state + object->offset);
            *(LONG *)(impl->base.device_state + object->offset) = value;
        }
        if (old == value) continue;

        if (!changed)
        {
            changed = TRUE;
            time = GetCurrentTime();
            seq = impl->base.dinput->evsequence++;
        }
        queue_event( iface, i, value, time, seq );
    }
    if (changed && impl->base.hEvent) SetEvent( impl->base.hEvent );
    if (ios_joystick_trace()) trace_state( impl, &state );
    LeaveCriticalSection( &impl->base.crit );

    return DI_OK;
}

static HRESULT ios_joystick_acquire( IDirectInputDevice8W *iface )
{
    return DI_OK;
}

static HRESULT ios_joystick_unacquire( IDirectInputDevice8W *iface )
{
    return DI_OK;
}

static HRESULT ios_joystick_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                          DWORD flags, enum_object_callback callback, void *context )
{
    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    return enum_objects( impl, filter, flags, callback, context );
}

static HRESULT ios_joystick_get_property( IDirectInputDevice8W *iface, DWORD property,
                                          DIPROPHEADER *header, const DIDEVICEOBJECTINSTANCEW *instance )
{
    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );

    switch (property)
    {
    case (DWORD_PTR)DIPROP_PRODUCTNAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, impl->base.instance.tszProductName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_INSTANCENAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, impl->base.instance.tszInstanceName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_VIDPID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = MAKELONG( IOS_JOYSTICK_VID, IOS_JOYSTICK_PID );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_JOYSTICKID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = impl->base.instance.guidInstance.Data3;
        return DI_OK;
    }
    }

    return DIERR_UNSUPPORTED;
}

static const struct dinput_device_vtbl ios_joystick_vtbl =
{
    NULL,                           /* destroy: nothing of its own */
    ios_joystick_poll,
    NULL,                           /* read: no device to wait on, the data is polled */
    ios_joystick_acquire,
    ios_joystick_unacquire,
    ios_joystick_enum_objects,
    ios_joystick_get_property,
    NULL,                           /* get_effect_info: no force feedback */
    NULL,                           /* create_effect */
    NULL,                           /* send_force_feedback_command */
    NULL,                           /* send_device_gain */
    NULL,                           /* enum_created_effect_objects */
};

/* E5: dinput's CreateDevice asks this first; DIERR_DEVICENOTREG lets the HID path try. */
HRESULT ios_joystick_create_device( struct dinput *dinput, const GUID *guid, IDirectInputDevice8W **out )
{
    static const DIPROPHEADER filter =
    {
        .dwSize = sizeof(filter),
        .dwHeaderSize = sizeof(filter),
        .dwHow = DIPH_DEVICE,
    };
    DIDEVICEINSTANCEW instance = {.dwSize = sizeof(instance)};
    GUID product = ios_joystick_product();
    struct ios_joystick *impl;
    HRESULT hr;

    *out = NULL;
    if (!IsEqualGUID( guid, &ios_joystick_guid ) && !IsEqualGUID( guid, &product )) return DIERR_DEVICENOTREG;
    if ((hr = ios_joystick_enum_device( 0, 0, &instance, dinput->dwVersion )) != DI_OK) return hr;

    TRACE( "dinput %p, guid %s, out %p\n", dinput, debugstr_guid( guid ), out );

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    dinput_device_init( &impl->base, &ios_joystick_vtbl, guid, dinput );
    impl->base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": ios_joystick.base.crit");
    impl->base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    impl->base.instance = instance;
    impl->base.caps.dwDevType = instance.dwDevType;
    impl->base.caps.dwFlags |= DIDC_POLLEDDEVICE | DIDC_POLLEDDATAFORMAT;

    if (FAILED(hr = dinput_device_init_device_format( &impl->base.IDirectInputDevice8W_iface )))
    {
        IDirectInputDevice_Release( &impl->base.IDirectInputDevice8W_iface );
        return hr;
    }
    enum_objects( impl, &filter, DIDFT_AXIS | DIDFT_POV, init_object_properties, NULL );

    *out = &impl->base.IDirectInputDevice8W_iface;
    return DI_OK;
}
