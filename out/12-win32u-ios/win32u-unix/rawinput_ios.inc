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

    if ((n = w32ios_diag_event( &w32ios_diag_lists, now )) <= 8)
        w32ios_log( "[rawinput] GetRawInputDeviceList %u (%s): %u devices, XInput slot devices listed:%s%s%s%s%s",
                    n, with_list ? "list" : "count", count, slots & 1 ? " 0" : "", slots & 2 ? " 1" : "",
                    slots & 4 ? " 2" : "", slots & 8 ? " 3" : "", slots ? "" : " none" );
    if ((n = w32ios_diag_due( &w32ios_diag_lists, now )))
        w32ios_log( "[rawinput] GetRawInputDeviceList: %u calls in 5 s, last %u devices", n, count );
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
