// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Included by rawinput.c (patches/S15-9-rawinput-diag.patch) after its device list, under
// WINE_IOS. Spec 15 clarification 9b: what a game does with raw input, for the XInput slot
// devices of spec 15 clarification 8. Each registration (the first 16), each device list call
// (the first 8, then a count per 5 s, naming the slot devices listed), and the first
// GetRawInputData and GetRawInputBuffer calls (then counts). Diagnostics only: nothing here
// changes an answer.

static struct w32ios_diag w32ios_diag_lists, w32ios_diag_data, w32ios_diag_buffers;

static void w32ios_diag_register( const RAWINPUTDEVICE *device )
{
    static unsigned int count;

    if (++count > 16) return;
    w32ios_log( "[rawinput] RegisterRawInputDevices: page %#x usage %#x flags %#x, %s",
                device->usUsagePage, device->usUsage, (unsigned int)device->dwFlags,
                device->hwndTarget ? "with a window" : "no window" );
}

/* `slots`: bit n set when XInput slot n's device was in the list. */
static void w32ios_diag_device_list( unsigned int count, unsigned int slots, BOOL with_list )
{
    uint64_t now = NtGetTickCount();
    unsigned int n;

    /* spec 15 clarification 10a: "slot 0" spelt out (build 11's "listed: 0" read as "none") */
    if ((n = w32ios_diag_event( &w32ios_diag_lists, now )) <= 8)
        w32ios_log( "[rawinput] GetRawInputDeviceList %u (%s): %u devices; XInput slot devices among them:%s%s%s%s%s",
                    n, with_list ? "list" : "count", count, slots & 1 ? " slot 0" : "", slots & 2 ? " slot 1" : "",
                    slots & 4 ? " slot 2" : "", slots & 8 ? " slot 3" : "", slots ? "" : " no slot device" );
    if ((n = w32ios_diag_due( &w32ios_diag_lists, now )))
        w32ios_log( "[rawinput] GetRawInputDeviceList: %u calls in 5 s, last %u devices", n, count );
}

/* Spec 15 clarification 10a: what GetRawInputDeviceInfo answered (the first 12 calls, then a
 * count per 5 s): the name, the device info (type, VID:PID, version, usage) or the preparsed
 * data's size. */
static struct w32ios_diag w32ios_diag_infos;

static void w32ios_diag_device_info( const struct device *device, UINT command, BOOL with_data,
                                     UINT asked, UINT len )
{
    uint64_t now = NtGetTickCount();
    char name[160];
    unsigned int n, i;

    if ((n = w32ios_diag_event( &w32ios_diag_infos, now )) <= 12)
    {
        switch (command)
        {
        case RIDI_DEVICENAME:
            for (i = 0; i < sizeof(name) - 1 && device->path[i]; i++)
                name[i] = device->path[i] < 0x80 ? (char)device->path[i] : '?';
            name[i] = 0;
            w32ios_log( "[rawinput] GetRawInputDeviceInfo %u: device %p RIDI_DEVICENAME (%s, %u chars asked): %u chars, %s",
                        n, device->handle, with_data ? "read" : "size", asked, len, name );
            break;
        case RIDI_DEVICEINFO:
            if (device->info.dwType == RIM_TYPEHID)
                w32ios_log( "[rawinput] GetRawInputDeviceInfo %u: device %p RIDI_DEVICEINFO (%s): HID %04x:%04x v%04x, usage page %#x usage %#x",
                            n, device->handle, with_data ? "read" : "size",
                            (unsigned int)device->info.hid.dwVendorId, (unsigned int)device->info.hid.dwProductId,
                            (unsigned int)device->info.hid.dwVersionNumber, device->info.hid.usUsagePage,
                            device->info.hid.usUsage );
            else
                w32ios_log( "[rawinput] GetRawInputDeviceInfo %u: device %p RIDI_DEVICEINFO (%s): type %u",
                            n, device->handle, with_data ? "read" : "size", (unsigned int)device->info.dwType );
            break;
        default:
            w32ios_log( "[rawinput] GetRawInputDeviceInfo %u: device %p command %#x (%s, %u asked): %u bytes",
                        n, device->handle, command, with_data ? "read" : "size", asked, len );
            break;
        }
    }
    if ((n = w32ios_diag_due( &w32ios_diag_infos, now )))
        w32ios_log( "[rawinput] GetRawInputDeviceInfo: %u calls in 5 s", n );
}

static void w32ios_diag_get_data( UINT command, UINT asked, UINT returned )
{
    uint64_t now = NtGetTickCount();
    unsigned int n;

    if ((n = w32ios_diag_event( &w32ios_diag_data, now )) <= 3)
        w32ios_log( "[rawinput] GetRawInputData %u: command %#x, size asked %u, returned %d",
                    n, command, asked, (int)returned );
    if ((n = w32ios_diag_due( &w32ios_diag_data, now )))
        w32ios_log( "[rawinput] GetRawInputData: %u calls in 5 s", n );
}

static void w32ios_diag_get_buffer( BOOL with_data, UINT asked, UINT next_size, UINT count )
{
    uint64_t now = NtGetTickCount();
    unsigned int n;

    if ((n = w32ios_diag_event( &w32ios_diag_buffers, now )) <= 3)
        w32ios_log( "[rawinput] GetRawInputBuffer %u (%s): size asked %u, returned %d messages, next size %u",
                    n, with_data ? "read" : "size", asked, (int)count, next_size );
    if ((n = w32ios_diag_due( &w32ios_diag_buffers, now )))
        w32ios_log( "[rawinput] GetRawInputBuffer: %u calls in 5 s", n );
}
