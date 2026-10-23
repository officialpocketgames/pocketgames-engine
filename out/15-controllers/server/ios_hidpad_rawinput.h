// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 8c: raw input for the XInput slot devices, the contract between this
// part's server library and the wineserver's message queue (spec 08, server/queue.c).
//
// On desktop Wine, hidclass.sys reads every HID device and hands each report to the server as
// a hardware message (WM_INPUT, RIM_TYPEHID), and announces devices with WM_INPUT_DEVICE_CHANGE;
// the server queues them for the processes that registered for the device's usage. The slot
// devices are served by the server itself, so nothing outside it sends those messages. The
// queue functions that do it are static in queue.c, so the queue offers them through this
// sink: spec 08 calls ios_hidpad_set_rawinput_sink once after ios_hidpad_server_init (through
// a weak reference, as it calls ios_hidpad_server_init). Without a sink no raw input messages
// are sent; everything else about the slot devices works.

#ifndef MYIOSDECK_IOS_HIDPAD_RAWINPUT_H
#define MYIOSDECK_IOS_HIDPAD_RAWINPUT_H

#define IOS_RAWINPUT_WM_INPUT_DEVICE_CHANGE 0x00FEu
#define IOS_RAWINPUT_WM_INPUT               0x00FFu
#define IOS_RAWINPUT_GIDC_ARRIVAL           1u
#define IOS_RAWINPUT_GIDC_REMOVAL           2u

struct ios_rawinput_sink
{
    /* 1 when some process registered (RegisterRawInputDevices) for raw input of this usage;
     * the server library sends reports only then, and asks again every 100 ms otherwise. */
    int (*wanted)( unsigned short usage_page, unsigned short usage );
    /* Queue `message` for the HID device whose raw input handle is `device` (the value its
     * IOCTL_HID_GET_WINE_RAWINPUT_HANDLE answers), as hidclass's hardware messages are queued:
     *   IOS_RAWINPUT_WM_INPUT: wparam 0 (RIM_INPUT), one report of `report_len` bytes (the read
     *     buffer's form: byte 0 the report id, 0 for these devices);
     *   IOS_RAWINPUT_WM_INPUT_DEVICE_CHANGE: wparam GIDC_ARRIVAL or GIDC_REMOVAL, no report. */
    void (*send)( unsigned int message, unsigned int wparam, unsigned int device,
                  unsigned short usage_page, unsigned short usage,
                  const void *report, unsigned int report_len );
};

/* Install (or with NULL remove) the sink; the server library keeps the pointer. */
void ios_hidpad_set_rawinput_sink( const struct ios_rawinput_sink *sink );

#endif
