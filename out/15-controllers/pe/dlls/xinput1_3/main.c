/*
 * The Wine project - Xinput Joystick Library
 * Copyright 2008 Andrew Fenn
 * Copyright 2018 Aric Stewart
 * Copyright 2021 Rémi Bernon for CodeWeavers
 *
 * Changed for MYIOSDECK (spec 15 Part D): every entry point asks the host's controller
 * slots (win32u, NtUserGetGamepadState, spec 12 Part F) first and falls back to the HID path
 * unchanged; XInputSetState / XInputEnable drive the host motors; the keystroke edge
 * detector moved to xinput_keystroke.h and is shared by both paths; motors left running are
 * stopped on process detach.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "winreg.h"
#include "wingdi.h"
#include "winnls.h"
#include "winternl.h"

#include "dbt.h"
#include "setupapi.h"
#include "devpkey.h"
#include "hidusage.h"
#include "ddk/hidsdi.h"
#include "initguid.h"
#include "devguid.h"
#include "xinput.h"
#include "ntuser.h"

#include "wine/debug.h"

#include "xinput_keystroke.h"

DEFINE_GUID(GUID_DEVINTERFACE_WINEXINPUT,0x6c53d5fd,0x6480,0x440f,0xb6,0x18,0x47,0x67,0x50,0xc5,0xe1,0xa6);

/* Not defined in the headers, used only by XInputGetStateEx */
#define XINPUT_GAMEPAD_GUIDE 0x0400

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

struct xinput_controller
{
    CRITICAL_SECTION crit;
    XINPUT_CAPABILITIES caps;
    XINPUT_STATE state;
    XINPUT_GAMEPAD last_keystroke;
    XINPUT_VIBRATION vibration;
    HANDLE device;
    WCHAR device_path[MAX_PATH];
    BOOL enabled;

    struct
    {
        PHIDP_PREPARSED_DATA preparsed;
        HIDP_CAPS caps;
        HIDP_VALUE_CAPS lx_caps;
        HIDP_VALUE_CAPS ly_caps;
        HIDP_VALUE_CAPS lt_caps;
        HIDP_VALUE_CAPS rx_caps;
        HIDP_VALUE_CAPS ry_caps;
        HIDP_VALUE_CAPS rt_caps;

        HANDLE read_event;
        OVERLAPPED read_ovl;

        char *input_report_buf;
        char *output_report_buf;
        char *feature_report_buf;

        BYTE haptics_report;
        HIDP_VALUE_CAPS haptics_rumble_caps;
        HIDP_VALUE_CAPS haptics_buzz_caps;
    } hid;
};

static struct xinput_controller controllers[XUSER_MAX_COUNT];
static CRITICAL_SECTION_DEBUG controller_critsect_debug[XUSER_MAX_COUNT] =
{
    {
        0, 0, &controllers[0].crit,
        { &controller_critsect_debug[0].ProcessLocksList, &controller_critsect_debug[0].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[0].crit") }
    },
    {
        0, 0, &controllers[1].crit,
        { &controller_critsect_debug[1].ProcessLocksList, &controller_critsect_debug[1].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[1].crit") }
    },
    {
        0, 0, &controllers[2].crit,
        { &controller_critsect_debug[2].ProcessLocksList, &controller_critsect_debug[2].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[2].crit") }
    },
    {
        0, 0, &controllers[3].crit,
        { &controller_critsect_debug[3].ProcessLocksList, &controller_critsect_debug[3].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[3].crit") }
    },
};

static struct xinput_controller controllers[XUSER_MAX_COUNT] =
{
    {{ &controller_critsect_debug[0], -1, 0, 0, 0, 0 }},
    {{ &controller_critsect_debug[1], -1, 0, 0, 0, 0 }},
    {{ &controller_critsect_debug[2], -1, 0, 0, 0, 0 }},
    {{ &controller_critsect_debug[3], -1, 0, 0, 0, 0 }},
};

static HMODULE xinput_instance;
static HANDLE start_event;
static HANDLE update_event;

/* MYIOSDECK (spec 15 Part D): the host's controller slots, answered by win32u
 * (NtUserCallTwoParam_GetGamepadState, spec 12 Part F): xinput_host.h. */
static CRITICAL_SECTION host_crit;
static CRITICAL_SECTION_DEBUG host_crit_debug =
{
    0, 0, &host_crit,
    { &host_crit_debug.ProcessLocksList, &host_crit_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": host_crit") }
};
static CRITICAL_SECTION host_crit = { &host_crit_debug, -1, 0, 0, 0, 0 };

#define XINPUT_HOST_CALL(index, op, buffer) NtUserGetGamepadState(index, op, buffer)
#define XINPUT_HOST_LOCK() EnterCriticalSection(&host_crit)
#define XINPUT_HOST_UNLOCK() LeaveCriticalSection(&host_crit)
#include "xinput_host.h"

static BOOL find_opened_device(const WCHAR *device_path, int *free_slot)
{
    int i;

    *free_slot = XUSER_MAX_COUNT;
    for (i = XUSER_MAX_COUNT; i > 0; i--)
    {
        if (!controllers[i - 1].device) *free_slot = i - 1;
        else if (!wcsicmp(device_path, controllers[i - 1].device_path)) return TRUE;
    }
    return FALSE;
}

static void check_value_caps(struct xinput_controller *controller, USHORT usage, HIDP_VALUE_CAPS *caps)
{
    switch (usage)
    {
    case HID_USAGE_GENERIC_X: controller->hid.lx_caps = *caps; break;
    case HID_USAGE_GENERIC_Y: controller->hid.ly_caps = *caps; break;
    case HID_USAGE_GENERIC_Z: controller->hid.lt_caps = *caps; break;
    case HID_USAGE_GENERIC_RX: controller->hid.rx_caps = *caps; break;
    case HID_USAGE_GENERIC_RY: controller->hid.ry_caps = *caps; break;
    case HID_USAGE_GENERIC_RZ: controller->hid.rt_caps = *caps; break;
    }
}

static void check_waveform_caps(struct xinput_controller *controller, HANDLE device, PHIDP_PREPARSED_DATA preparsed,
                                HIDP_LINK_COLLECTION_NODE *collections, HIDP_VALUE_CAPS *caps)
{
    USHORT count, report_len = controller->hid.caps.FeatureReportByteLength;
    char *report_buf = controller->hid.feature_report_buf;
    ULONG parent = caps->LinkCollection, waveform = 0;
    HIDP_VALUE_CAPS value_caps;
    USAGE_AND_PAGE phy_usages;
    NTSTATUS status;

    while (collections[parent].LinkUsagePage != HID_USAGE_PAGE_HAPTICS ||
           collections[parent].LinkUsage != HID_USAGE_HAPTICS_SIMPLE_CONTROLLER)
        if (!(parent = collections[parent].Parent)) break;

    if (collections[parent].LinkUsagePage != HID_USAGE_PAGE_HAPTICS ||
        collections[parent].LinkUsage != HID_USAGE_HAPTICS_SIMPLE_CONTROLLER)
    {
        WARN("Failed to find haptics simple controller collection\n");
        return;
    }
    phy_usages.UsagePage = collections[collections[parent].Parent].LinkUsagePage;
    phy_usages.Usage = collections[collections[parent].Parent].LinkUsage;

    status = HidP_InitializeReportForID(HidP_Feature, caps->ReportID, preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_InitializeReportForID returned %#lx\n", status);
    if (!HidD_GetFeature(device, report_buf, report_len))
    {
        WARN("Failed to get waveform list report, error %lu\n", GetLastError());
        return;
    }

    status = HidP_GetUsageValue(HidP_Feature, caps->UsagePage, caps->LinkCollection, caps->NotRange.Usage,
                                &waveform, preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue returned %#lx\n", status);

    count = 1;
    status = HidP_GetSpecificValueCaps(HidP_Output, HID_USAGE_PAGE_HAPTICS, parent, HID_USAGE_HAPTICS_INTENSITY,
                                       &value_caps, &count, preparsed);
    if (status != HIDP_STATUS_SUCCESS || !count) WARN("Failed to get waveform intensity caps, status %#lx\n", status);
    else if (phy_usages.UsagePage == HID_USAGE_PAGE_GENERIC && phy_usages.Usage == HID_USAGE_GENERIC_Z)
        TRACE( "Ignoring left rumble caps\n" );
    else if (phy_usages.UsagePage == HID_USAGE_PAGE_GENERIC && phy_usages.Usage == HID_USAGE_GENERIC_RZ)
        TRACE( "Ignoring right rumble caps\n" );
    else if (waveform == HID_USAGE_HAPTICS_WAVEFORM_RUMBLE)
    {
        TRACE("Found rumble caps, report %u collection %u\n", value_caps.ReportID, value_caps.LinkCollection);
        controller->hid.haptics_report = value_caps.ReportID;
        controller->hid.haptics_rumble_caps = value_caps;
    }
    else if (waveform == HID_USAGE_HAPTICS_WAVEFORM_BUZZ)
    {
        TRACE("Found buzz caps, report %u collection %u\n", value_caps.ReportID, value_caps.LinkCollection);
        controller->hid.haptics_report = value_caps.ReportID;
        controller->hid.haptics_buzz_caps = value_caps;
    }
    else FIXME("Unsupported waveform type %#lx\n", waveform);
}

static BOOL controller_check_caps(struct xinput_controller *controller, HANDLE device, PHIDP_PREPARSED_DATA preparsed)
{
    USHORT caps_count = 0, waveform_caps_count = 0;
    XINPUT_CAPABILITIES *caps = &controller->caps;
    HIDP_LINK_COLLECTION_NODE *collections;
    HIDP_VALUE_CAPS waveform_caps[8];
    HIDP_BUTTON_CAPS *button_caps;
    ULONG collections_count = 0;
    HIDP_VALUE_CAPS *value_caps;
    int i, u, button_count = 0;
    NTSTATUS status;

    /* Count buttons */
    memset(caps, 0, sizeof(XINPUT_CAPABILITIES));

    if (!(button_caps = malloc(sizeof(*button_caps) * controller->hid.caps.NumberInputButtonCaps))) return FALSE;
    status = HidP_GetButtonCaps(HidP_Input, button_caps, &controller->hid.caps.NumberInputButtonCaps, preparsed);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetButtonCaps returned %#lx\n", status);
    else for (i = 0; i < controller->hid.caps.NumberInputButtonCaps; i++)
    {
        if (button_caps[i].UsagePage != HID_USAGE_PAGE_BUTTON)
            continue;
        if (button_caps[i].IsRange)
            button_count = max(button_count, button_caps[i].Range.UsageMax);
        else
            button_count = max(button_count, button_caps[i].NotRange.Usage);
    }
    free(button_caps);
    if (button_count < 11)
        WARN("Too few buttons, continuing anyway\n");
    caps->Gamepad.wButtons = 0xffff;

    if (!(value_caps = malloc(sizeof(*value_caps) * controller->hid.caps.NumberInputValueCaps))) return FALSE;
    status = HidP_GetValueCaps(HidP_Input, value_caps, &controller->hid.caps.NumberInputValueCaps, preparsed);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetValueCaps returned %#lx\n", status);
    else for (i = 0; i < controller->hid.caps.NumberInputValueCaps; i++)
    {
        HIDP_VALUE_CAPS *caps = value_caps + i;
        if (caps->UsagePage != HID_USAGE_PAGE_GENERIC) continue;
        if (!caps->IsRange) check_value_caps(controller, caps->NotRange.Usage, caps);
        else for (u = caps->Range.UsageMin; u <=caps->Range.UsageMax; u++) check_value_caps(controller, u, value_caps + i);
    }
    free(value_caps);

    if (!controller->hid.lt_caps.UsagePage) WARN("Missing axis LeftTrigger\n");
    else caps->Gamepad.bLeftTrigger = (1u << (sizeof(caps->Gamepad.bLeftTrigger) + 1)) - 1;
    if (!controller->hid.rt_caps.UsagePage) WARN("Missing axis RightTrigger\n");
    else caps->Gamepad.bRightTrigger = (1u << (sizeof(caps->Gamepad.bRightTrigger) + 1)) - 1;
    if (!controller->hid.lx_caps.UsagePage) WARN("Missing axis ThumbLX\n");
    else caps->Gamepad.sThumbLX = (1u << (sizeof(caps->Gamepad.sThumbLX) + 1)) - 1;
    if (!controller->hid.ly_caps.UsagePage) WARN("Missing axis ThumbLY\n");
    else caps->Gamepad.sThumbLY = (1u << (sizeof(caps->Gamepad.sThumbLY) + 1)) - 1;
    if (!controller->hid.rx_caps.UsagePage) WARN("Missing axis ThumbRX\n");
    else caps->Gamepad.sThumbRX = (1u << (sizeof(caps->Gamepad.sThumbRX) + 1)) - 1;
    if (!controller->hid.ry_caps.UsagePage) WARN("Missing axis ThumbRY\n");
    else caps->Gamepad.sThumbRY = (1u << (sizeof(caps->Gamepad.sThumbRY) + 1)) - 1;

    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;

    collections_count = controller->hid.caps.NumberLinkCollectionNodes;
    if (!(collections = malloc(sizeof(*collections) * controller->hid.caps.NumberLinkCollectionNodes))) return FALSE;
    status = HidP_GetLinkCollectionNodes(collections, &collections_count, preparsed);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetLinkCollectionNodes returned %#lx\n", status);
    else for (i = 0; i < collections_count; ++i)
    {
        if (collections[i].LinkUsagePage != HID_USAGE_PAGE_HAPTICS) continue;
        if (collections[i].LinkUsage == HID_USAGE_HAPTICS_WAVEFORM_LIST)
        {
            caps_count = ARRAY_SIZE(waveform_caps) - waveform_caps_count;
            value_caps = waveform_caps + waveform_caps_count;
            status = HidP_GetSpecificValueCaps(HidP_Feature, HID_USAGE_PAGE_ORDINAL, i, 0, value_caps, &caps_count, preparsed);
            if (status == HIDP_STATUS_SUCCESS) waveform_caps_count += caps_count;
        }
    }
    for (i = 0; i < waveform_caps_count; ++i) check_waveform_caps(controller, device, preparsed, collections, waveform_caps + i);
    free(collections);

    if (controller->hid.haptics_rumble_caps.UsagePage ||
        controller->hid.haptics_buzz_caps.UsagePage)
    {
        caps->Flags |= XINPUT_CAPS_FFB_SUPPORTED;
        caps->Vibration.wLeftMotorSpeed = 255;
        caps->Vibration.wRightMotorSpeed = 255;
    }

    return TRUE;
}

static DWORD HID_set_state(struct xinput_controller *controller, XINPUT_VIBRATION *state)
{
    ULONG report_len = controller->hid.caps.OutputReportByteLength;
    PHIDP_PREPARSED_DATA preparsed = controller->hid.preparsed;
    char *report_buf = controller->hid.output_report_buf;
    BOOL ret, update_rumble, update_buzz;
    USHORT collection;
    NTSTATUS status;
    BYTE report_id;

    if (!(controller->caps.Flags & XINPUT_CAPS_FFB_SUPPORTED)) return ERROR_SUCCESS;

    update_rumble = (controller->vibration.wLeftMotorSpeed != state->wLeftMotorSpeed);
    controller->vibration.wLeftMotorSpeed = state->wLeftMotorSpeed;
    update_buzz = (controller->vibration.wRightMotorSpeed != state->wRightMotorSpeed);
    controller->vibration.wRightMotorSpeed = state->wRightMotorSpeed;

    if (!controller->enabled) return ERROR_SUCCESS;
    if (!update_rumble && !update_buzz) return ERROR_SUCCESS;

    report_id = controller->hid.haptics_report;
    status = HidP_InitializeReportForID(HidP_Output, report_id, preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_InitializeReportForID returned %#lx\n", status);

    collection = controller->hid.haptics_rumble_caps.LinkCollection;
    status = HidP_SetUsageValue(HidP_Output, HID_USAGE_PAGE_HAPTICS, collection, HID_USAGE_HAPTICS_INTENSITY,
                                state->wLeftMotorSpeed, preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_SetUsageValue INTENSITY returned %#lx\n", status);

    collection = controller->hid.haptics_buzz_caps.LinkCollection;
    status = HidP_SetUsageValue(HidP_Output, HID_USAGE_PAGE_HAPTICS, collection, HID_USAGE_HAPTICS_INTENSITY,
                                state->wRightMotorSpeed, preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_SetUsageValue INTENSITY returned %#lx\n", status);

    ret = HidD_SetOutputReport(controller->device, report_buf, report_len);
    if (!ret) WARN("HidD_SetOutputReport failed with error %lu\n", GetLastError());
    return 0;

    return ERROR_SUCCESS;
}

static void controller_destroy(struct xinput_controller *controller, BOOL already_removed);

static void controller_enable(struct xinput_controller *controller)
{
    ULONG report_len = controller->hid.caps.InputReportByteLength;
    char *report_buf = controller->hid.input_report_buf;
    XINPUT_VIBRATION state = controller->vibration;
    BOOL ret;

    if (controller->enabled) return;
    if (controller->caps.Flags & XINPUT_CAPS_FFB_SUPPORTED) HID_set_state(controller, &state);
    controller->enabled = TRUE;

    memset(&controller->hid.read_ovl, 0, sizeof(controller->hid.read_ovl));
    controller->hid.read_ovl.hEvent = controller->hid.read_event;
    ret = ReadFile(controller->device, report_buf, report_len, NULL, &controller->hid.read_ovl);
    if (!ret && GetLastError() != ERROR_IO_PENDING) controller_destroy(controller, TRUE);
    else SetEvent(update_event);
}

static void controller_disable(struct xinput_controller *controller)
{
    XINPUT_VIBRATION state = {0};

    if (!controller->enabled) return;
    if (controller->caps.Flags & XINPUT_CAPS_FFB_SUPPORTED) HID_set_state(controller, &state);
    controller->enabled = FALSE;

    CancelIoEx(controller->device, &controller->hid.read_ovl);
    WaitForSingleObject(controller->hid.read_ovl.hEvent, INFINITE);
    SetEvent(update_event);
}

static BOOL controller_init(struct xinput_controller *controller, PHIDP_PREPARSED_DATA preparsed,
                            HIDP_CAPS *caps, HANDLE device, const WCHAR *device_path)
{
    HANDLE event = NULL;

    controller->hid.caps = *caps;
    if (!(controller->hid.feature_report_buf = calloc(1, controller->hid.caps.FeatureReportByteLength))) goto failed;
    if (!controller_check_caps(controller, device, preparsed)) goto failed;
    if (!(event = CreateEventW(NULL, TRUE, FALSE, NULL))) goto failed;

    TRACE("Found gamepad %s\n", debugstr_w(device_path));

    controller->hid.preparsed = preparsed;
    controller->hid.read_event = event;
    if (!(controller->hid.input_report_buf = calloc(1, controller->hid.caps.InputReportByteLength))) goto failed;
    if (!(controller->hid.output_report_buf = calloc(1, controller->hid.caps.OutputReportByteLength))) goto failed;

    memset(&controller->state, 0, sizeof(controller->state));
    memset(&controller->vibration, 0, sizeof(controller->vibration));
    lstrcpynW(controller->device_path, device_path, MAX_PATH);
    controller->enabled = FALSE;

    EnterCriticalSection(&controller->crit);
    controller->device = device;
    controller_enable(controller);
    LeaveCriticalSection(&controller->crit);
    return TRUE;

failed:
    free(controller->hid.input_report_buf);
    free(controller->hid.output_report_buf);
    free(controller->hid.feature_report_buf);
    memset(&controller->hid, 0, sizeof(controller->hid));
    CloseHandle(event);
    return FALSE;
}

static void get_registry_keys(HKEY *defkey, HKEY *appkey)
{
    WCHAR buffer[MAX_PATH + 26], *name = buffer, *tmp;
    DWORD len;
    HKEY hkey;

    *appkey = 0;
    if (RegOpenKeyW(HKEY_CURRENT_USER, L"Software\\Wine\\DirectInput\\Joysticks", defkey))
        *defkey = 0;

    if (!(len = GetModuleFileNameW(0, buffer, MAX_PATH)) || len >= MAX_PATH)
        return;

    if (!RegOpenKeyW(HKEY_CURRENT_USER, L"Software\\Wine\\AppDefaults", &hkey))
    {
        if ((tmp = wcsrchr(name, '/'))) name = tmp + 1;
        if ((tmp = wcsrchr(name, '\\'))) name = tmp + 1;
        wcscat(name, L"\\DirectInput\\Joysticks");
        if (RegOpenKeyW(hkey, name, appkey)) *appkey = 0;
        RegCloseKey(hkey);
    }
}

static BOOL device_is_overridden(HANDLE device)
{
    WCHAR name[MAX_PATH], buffer[MAX_PATH];
    DWORD size = sizeof(buffer);
    BOOL disable = FALSE;
    HKEY defkey, appkey;

    if (!HidD_GetProductString(device, name, MAX_PATH)) return FALSE;

    get_registry_keys(&defkey, &appkey);
    if (!defkey && !appkey) return FALSE;
    if ((appkey && !RegQueryValueExW(appkey, name, 0, NULL, (LPBYTE)buffer, &size)) ||
        (defkey && !RegQueryValueExW(defkey, name, 0, NULL, (LPBYTE)buffer, &size)))
    {
        if ((disable = !wcscmp(buffer, L"override")))
            TRACE("Disabling gamepad '%s' based on registry key.\n", debugstr_w(name));
    }

    if (appkey) RegCloseKey(appkey);
    if (defkey) RegCloseKey(defkey);
    return disable;
}

static BOOL try_add_device(const WCHAR *device_path)
{
    SP_DEVICE_INTERFACE_DATA iface = {sizeof(iface)};
    PHIDP_PREPARSED_DATA preparsed;
    HIDP_CAPS caps;
    NTSTATUS status;
    HANDLE device;
    int i;

    if (find_opened_device(device_path, &i)) return TRUE; /* already opened */
    if (i == XUSER_MAX_COUNT) return FALSE; /* no more slots */

    device = CreateFileW(device_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    if (device == INVALID_HANDLE_VALUE) return TRUE;

    preparsed = NULL;
    if (!HidD_GetPreparsedData(device, &preparsed))
        WARN("ignoring HID device, HidD_GetPreparsedData failed with error %lu\n", GetLastError());
    else if ((status = HidP_GetCaps(preparsed, &caps)) != HIDP_STATUS_SUCCESS)
        WARN("ignoring HID device, HidP_GetCaps returned %#lx\n", status);
    else if (caps.UsagePage != HID_USAGE_PAGE_GENERIC)
        WARN("ignoring HID device, unsupported usage page %04x\n", caps.UsagePage);
    else if (caps.Usage != HID_USAGE_GENERIC_GAMEPAD && caps.Usage != HID_USAGE_GENERIC_JOYSTICK &&
             caps.Usage != HID_USAGE_GENERIC_MULTI_AXIS_CONTROLLER)
        WARN("ignoring HID device, unsupported usage %04x:%04x\n", caps.UsagePage, caps.Usage);
    else if (device_is_overridden(device))
        WARN("ignoring HID device, overridden for dinput\n");
    else if (!controller_init(&controllers[i], preparsed, &caps, device, device_path))
        WARN("ignoring HID device, failed to initialize\n");
    else
        return TRUE;

    CloseHandle(device);
    HidD_FreePreparsedData(preparsed);
    return TRUE;
}

static void try_remove_device(const WCHAR *device_path)
{
    int i;

    if (find_opened_device(device_path, &i))
        controller_destroy(&controllers[i], TRUE);
}

static void update_controller_list(void)
{
    char buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof(WCHAR)];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    SP_DEVICE_INTERFACE_DATA iface = {sizeof(iface)};
    HDEVINFO set;
    DWORD idx;
    GUID guid;

    guid = GUID_DEVINTERFACE_WINEXINPUT;

    set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    detail->cbSize = sizeof(*detail);

    idx = 0;
    while (SetupDiEnumDeviceInterfaces(set, NULL, &guid, idx++, &iface))
    {
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, sizeof(buffer), NULL, NULL))
            continue;
        if (!try_add_device(detail->DevicePath))
            break;
    }

    SetupDiDestroyDeviceInfoList(set);
}

static void controller_destroy(struct xinput_controller *controller, BOOL already_removed)
{
    EnterCriticalSection(&controller->crit);

    if (controller->device)
    {
        if (!already_removed) controller_disable(controller);
        CloseHandle(controller->device);
        controller->device = NULL;

        free(controller->hid.input_report_buf);
        free(controller->hid.output_report_buf);
        free(controller->hid.feature_report_buf);
        HidD_FreePreparsedData(controller->hid.preparsed);
        memset(&controller->hid, 0, sizeof(controller->hid));
    }

    LeaveCriticalSection(&controller->crit);
}

static LONG sign_extend(ULONG value, const HIDP_VALUE_CAPS *caps)
{
    UINT sign = 1 << (caps->BitSize - 1);
    if (sign <= 1 || caps->LogicalMin >= 0) return value;
    return value - ((value & sign) << 1);
}

static LONG scale_value(ULONG value, const HIDP_VALUE_CAPS *caps, LONG min, LONG max)
{
    LONG tmp = sign_extend(value, caps);
    if (caps->LogicalMin > caps->LogicalMax) return 0;
    if (caps->LogicalMin > tmp || caps->LogicalMax < tmp) return 0;
    return min + MulDiv(tmp - caps->LogicalMin, max - min, caps->LogicalMax - caps->LogicalMin);
}

static void read_controller_state(struct xinput_controller *controller)
{
    ULONG read_len, report_len = controller->hid.caps.InputReportByteLength;
    char *report_buf = controller->hid.input_report_buf;
    XINPUT_STATE state;
    NTSTATUS status;
    USAGE buttons[11];
    ULONG i, button_length, value;
    BOOL ret;

    if (!GetOverlappedResult(controller->device, &controller->hid.read_ovl, &read_len, TRUE))
    {
        if (GetLastError() == ERROR_OPERATION_ABORTED) return;
        if (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_INVALID_HANDLE ||
            GetLastError() == ERROR_DEVICE_NOT_CONNECTED)
        {
            controller_destroy(controller, TRUE);
        }
        else ERR("Failed to read input report, GetOverlappedResult failed with error %lu\n", GetLastError());
        return;
    }

    button_length = ARRAY_SIZE(buttons);
    status = HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, buttons, &button_length, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsages HID_USAGE_PAGE_BUTTON returned %#lx\n", status);

    state.Gamepad.wButtons = 0;
    for (i = 0; i < button_length; i++)
    {
        switch (buttons[i])
        {
        case 1: state.Gamepad.wButtons |= XINPUT_GAMEPAD_A; break;
        case 2: state.Gamepad.wButtons |= XINPUT_GAMEPAD_B; break;
        case 3: state.Gamepad.wButtons |= XINPUT_GAMEPAD_X; break;
        case 4: state.Gamepad.wButtons |= XINPUT_GAMEPAD_Y; break;
        case 5: state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
        case 6: state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
        case 7: state.Gamepad.wButtons |= XINPUT_GAMEPAD_BACK; break;
        case 8: state.Gamepad.wButtons |= XINPUT_GAMEPAD_START; break;
        case 9: state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB; break;
        case 10: state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; break;
        }
    }

    button_length = ARRAY_SIZE(buttons);
    status = HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_VENDOR_DEFINED_BEGIN, 0, buttons, &button_length, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsages HID_USAGE_PAGE_VENDOR_DEFINED_BEGIN returned %#lx\n", status);
    if (button_length) state.Gamepad.wButtons |= XINPUT_GAMEPAD_GUIDE;

    status = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_HATSWITCH, &value, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_HATSWITCH returned %#lx\n", status);
    else switch (value)
    {
    /* 8 1 2
     * 7 0 3
     * 6 5 4 */
    case 0: break;
    case 1: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP; break;
    case 2: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 3: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 4: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 5: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 6: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 7: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 8: state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP; break;
    }

    status = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_X, &value, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_X returned %#lx\n", status);
    else state.Gamepad.sThumbLX = scale_value(value, &controller->hid.lx_caps, -32768, 32767);

    status = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_Y, &value, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_Y returned %#lx\n", status);
    else state.Gamepad.sThumbLY = scale_value(value, &controller->hid.ly_caps, -32768, 32767);

    status = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_RX, &value, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_RX returned %#lx\n", status);
    else state.Gamepad.sThumbRX = scale_value(value, &controller->hid.rx_caps, -32768, 32767);

    status = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_RY, &value, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_RY returned %#lx\n", status);
    else state.Gamepad.sThumbRY = scale_value(value, &controller->hid.ry_caps, -32768, 32767);

    status = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_RZ, &value, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_RZ returned %#lx\n", status);
    else state.Gamepad.bRightTrigger = scale_value(value, &controller->hid.rt_caps, 0, 255);

    status = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_Z, &value, controller->hid.preparsed, report_buf, report_len);
    if (status != HIDP_STATUS_SUCCESS) WARN("HidP_GetUsageValue HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_Z returned %#lx\n", status);
    else state.Gamepad.bLeftTrigger = scale_value(value, &controller->hid.lt_caps, 0, 255);

    EnterCriticalSection(&controller->crit);
    if (controller->enabled)
    {
        state.dwPacketNumber = controller->state.dwPacketNumber + 1;
        controller->state = state;
        memset(&controller->hid.read_ovl, 0, sizeof(controller->hid.read_ovl));
        controller->hid.read_ovl.hEvent = controller->hid.read_event;
        ret = ReadFile(controller->device, report_buf, report_len, NULL, &controller->hid.read_ovl);
        if (!ret && GetLastError() != ERROR_IO_PENDING) controller_destroy(controller, TRUE);
    }
    LeaveCriticalSection(&controller->crit);
}

static LRESULT CALLBACK xinput_devnotify_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_DEVICECHANGE)
    {
        DEV_BROADCAST_DEVICEINTERFACE_W *iface = (DEV_BROADCAST_DEVICEINTERFACE_W *)lparam;
        if (wparam == DBT_DEVICEARRIVAL) try_add_device(iface->dbcc_name);
        if (wparam == DBT_DEVICEREMOVECOMPLETE) try_remove_device(iface->dbcc_name);
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI hid_update_thread_proc(void *param)
{
    struct xinput_controller *devices[XUSER_MAX_COUNT + 1];
    HANDLE events[XUSER_MAX_COUNT + 1];
    DWORD i, count = 1, ret = WAIT_TIMEOUT;
    DEV_BROADCAST_DEVICEINTERFACE_W filter =
    {
        .dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE_W),
        .dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE,
        .dbcc_classguid = GUID_DEVINTERFACE_WINEXINPUT,
    };
    WNDCLASSEXW cls =
    {
        .cbSize = sizeof(WNDCLASSEXW),
        .hInstance = xinput_instance,
        .lpszClassName = L"__wine_xinput_devnotify",
        .lpfnWndProc = xinput_devnotify_wndproc,
    };
    HDEVNOTIFY notif;
    HWND hwnd;
    MSG msg;

    SetThreadDescription(GetCurrentThread(), L"wine_xinput_hid_update");

    RegisterClassExW(&cls);
    hwnd = CreateWindowExW(0, cls.lpszClassName, NULL, 0, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, NULL, NULL);
    notif = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    update_controller_list();
    SetEvent(start_event);

    do
    {
        if (ret == count) while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (ret == WAIT_TIMEOUT) update_controller_list();
        if (ret < count - 1) read_controller_state(devices[ret]);

        count = 0;
        for (i = 0; i < XUSER_MAX_COUNT; ++i)
        {
            if (!controllers[i].device) continue;
            EnterCriticalSection(&controllers[i].crit);
            if (controllers[i].enabled)
            {
                devices[count] = controllers + i;
                events[count] = controllers[i].hid.read_event;
                count++;
            }
            LeaveCriticalSection(&controllers[i].crit);
        }
        events[count++] = update_event;
    }
    while ((ret = MsgWaitForMultipleObjectsEx(count, events, 2000, QS_ALLINPUT, MWMO_ALERTABLE)) <= count ||
            ret == WAIT_TIMEOUT);

    ERR("wait failed in the update thread, ret %lu, error %lu\n", ret, GetLastError());

    UnregisterDeviceNotification(notif);
    DestroyWindow(hwnd);
    UnregisterClassW(cls.lpszClassName, xinput_instance);

    FreeLibraryAndExitThread(xinput_instance, ret);
}

static BOOL WINAPI start_update_thread_once( INIT_ONCE *once, void *param, void **context )
{
    HANDLE thread;
    HMODULE module;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (void*)hid_update_thread_proc, &module))
        WARN("Failed to increase module's reference count, error: %lu\n", GetLastError());

    start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!start_event) ERR("failed to create start event, error %lu\n", GetLastError());

    update_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!update_event) ERR("failed to create update event, error %lu\n", GetLastError());

    thread = CreateThread(NULL, 0, hid_update_thread_proc, NULL, 0, NULL);
    if (!thread) ERR("failed to create update thread, error %lu\n", GetLastError());
    CloseHandle(thread);

    WaitForSingleObject(start_event, INFINITE);
    return TRUE;
}

static void start_update_thread(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init_once, start_update_thread_once, NULL, NULL);
}

static BOOL controller_lock(struct xinput_controller *controller)
{
    if (!controller->device) return FALSE;

    EnterCriticalSection(&controller->crit);

    if (!controller->device)
    {
        LeaveCriticalSection(&controller->crit);
        return FALSE;
    }

    return TRUE;
}

static void controller_unlock(struct xinput_controller *controller)
{
    LeaveCriticalSection(&controller->crit);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    TRACE("inst %p, reason %lu, reserved %p.\n", inst, reason, reserved);

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        xinput_instance = inst;
        DisableThreadLibraryCalls(inst);
        break;
    case DLL_PROCESS_DETACH:
        /* MYIOSDECK (spec 15 Part D5) */
        host_stop_motors();
        break;
    }
    return TRUE;
}

void WINAPI DECLSPEC_HOTPATCH XInputEnable(BOOL enable)
{
    int index;

    TRACE("enable %d.\n", enable);

    /* Setting to false will stop messages from XInputSetState being sent
    to the controllers. Setting to true will send the last vibration
    value (sent to XInputSetState) to the controller and allow messages to
    be sent */
    host_enable(enable);   /* MYIOSDECK: the host slots first */

    start_update_thread();

    for (index = 0; index < XUSER_MAX_COUNT; index++)
    {
        if (!controller_lock(&controllers[index])) continue;
        if (enable) controller_enable(&controllers[index]);
        else controller_disable(&controllers[index]);
        controller_unlock(&controllers[index]);
    }
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    DWORD ret;

    TRACE("index %lu, vibration %p.\n", index, vibration);

    /* MYIOSDECK: a connected host slot stores the motors and sends them */
    if (host_set_state(index, vibration)) return ERROR_SUCCESS;

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_lock(&controllers[index])) return ERROR_DEVICE_NOT_CONNECTED;

    ret = HID_set_state(&controllers[index], vibration);

    controller_unlock(&controllers[index]);

    return ret;
}

/* Some versions of SteamOverlayRenderer hot-patch XInputGetStateEx() and call
 * XInputGetState() in the hook, so we need a wrapper. */
static DWORD xinput_get_state(DWORD index, XINPUT_STATE *state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;

    /* MYIOSDECK: a connected host slot answers at once */
    if (host_get_state(index, state)) return ERROR_SUCCESS;

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_lock(&controllers[index])) return ERROR_DEVICE_NOT_CONNECTED;

    *state = controllers[index].state;
    controller_unlock(&controllers[index]);

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetState(DWORD index, XINPUT_STATE *state)
{
    DWORD ret;

    TRACE("index %lu, state %p.\n", index, state);

    ret = xinput_get_state(index, state);
    if (ret != ERROR_SUCCESS) return ret;

    /* The main difference between this and the Ex version is the media guide button */
    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    TRACE("index %lu, state %p.\n", index, state);

    return xinput_get_state(index, state);
}

/* MYIOSDECK (spec 15 Part D4): the edge detector is xinput_keystroke.h, shared with the host
 * slots; a connected host slot answers first. */
static DWORD check_for_keystroke(const DWORD index, XINPUT_KEYSTROKE *keystroke)
{
    struct xinput_controller *controller = &controllers[index];
    DWORD ret = ERROR_EMPTY;

    if (host_get_keystroke(index, keystroke, &ret)) return ret;

    if (!controller_lock(controller)) return ERROR_DEVICE_NOT_CONNECTED;

    ret = xinput_keystroke_edge(index, &controller->state.Gamepad, &controller->last_keystroke, keystroke);

    controller_unlock(controller);

    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{
    TRACE("index %lu, reserved %lu, keystroke %p.\n", index, reserved, keystroke);

    if (index >= XUSER_MAX_COUNT && index != XUSER_INDEX_ANY) return ERROR_BAD_ARGUMENTS;

    if (index == XUSER_INDEX_ANY)
    {
        int i;
        for (i = 0; i < XUSER_MAX_COUNT; ++i)
            if (check_for_keystroke(i, keystroke) == ERROR_SUCCESS)
                return ERROR_SUCCESS;
        return ERROR_EMPTY;
    }

    return check_for_keystroke(index, keystroke);
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities)
{
    XINPUT_CAPABILITIES_EX caps_ex;
    DWORD ret;

    ret = XInputGetCapabilitiesEx(1, index, flags, &caps_ex);

    if (!ret) *capabilities = caps_ex.Capabilities;

    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render_guid, GUID *capture_guid)
{
    FIXME("index %lu, render_guid %s, capture_guid %s stub!\n", index, debugstr_guid(render_guid),
          debugstr_guid(capture_guid));

    if (index >= XUSER_MAX_COUNT || !render_guid || !capture_guid) return ERROR_BAD_ARGUMENTS;

    /* MYIOSDECK: a host slot has no headset */
    if (host_get_dsound_guids(index, render_guid, capture_guid)) return ERROR_SUCCESS;

    if (!controllers[index].device) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION* battery)
{
    static int once;
    DWORD ret;

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;

    /* MYIOSDECK (spec 15 clarification 2): a host slot's battery is unknown, no headset */
    if (host_get_battery(index, type, battery, &ret)) return ret;

    if (!once++) FIXME("index %lu, type %u, battery %p.\n", index, type, battery);
    if (!controllers[index].device) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    HIDD_ATTRIBUTES attr;
    DWORD ret = ERROR_SUCCESS;

    TRACE("unk %lu, index %lu, flags %#lx, capabilities %p.\n", unk, index, flags, caps);

    /* MYIOSDECK: a connected host slot answers with win32u's Caps operation */
    if (host_get_capabilities_ex(index, flags, caps, &ret)) return ret;

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;

    if (!controller_lock(&controllers[index])) return ERROR_DEVICE_NOT_CONNECTED;

    if (flags & XINPUT_FLAG_GAMEPAD && controllers[index].caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD)
        ret = ERROR_DEVICE_NOT_CONNECTED;
    else if (!HidD_GetAttributes(controllers[index].device, &attr))
        ret = ERROR_DEVICE_NOT_CONNECTED;
    else
    {
        caps->Capabilities = controllers[index].caps;
        caps->VendorId = attr.VendorID;
        caps->ProductId = attr.ProductID;
        caps->VersionNumber = attr.VersionNumber;
    }

    controller_unlock(&controllers[index]);

    return ret;
}
