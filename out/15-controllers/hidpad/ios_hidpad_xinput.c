// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 8: each XInput host slot as the HID device Windows gives an XInput pad,
// the "XInput HID-compatible" game controller collection. Pure C (no Wine headers, no
// allocation, no I/O), like ios_hidpad.c. The collection's layout (X Y, Rx Ry, both triggers
// on Z, 10 buttons, a hat, no report ids, no output or feature reports) is the one the spec
// names; upstream Wine 11.4's dlls/winexinput.sys models the same collection, and its field
// order and the trigger formula were checked against it (see PROVENANCE.md). The descriptor
// bytes and the code are our own.

#include <string.h>

#include "ios_hidpad.h"

#define PAD_DPAD_UP        0x0001u
#define PAD_DPAD_DOWN      0x0002u
#define PAD_DPAD_LEFT      0x0004u
#define PAD_DPAD_RIGHT     0x0008u
#define PAD_START          0x0010u
#define PAD_BACK           0x0020u
#define PAD_LEFT_THUMB     0x0040u
#define PAD_RIGHT_THUMB    0x0080u
#define PAD_LEFT_SHOULDER  0x0100u
#define PAD_RIGHT_SHOULDER 0x0200u
#define PAD_A              0x1000u
#define PAD_B              0x2000u
#define PAD_X              0x4000u
#define PAD_Y              0x8000u

/* No report id; 14 bytes of data (the read buffer has a leading 0 byte, 15 in all):
 *   [1..2] X  [3..4] Y  [5..6] Rx  [7..8] Ry  [9..10] Z      16-bit, 0..65535, little endian
 *   [11..12] buttons 1-10 in bits 0-9, the hat (1-8, 0 = null) in bits 10-13, bits 14-15 pad
 *   [13..14] padding */
const uint8_t ios_hidpad_xinput_descriptor[] =
{
    0x05, 0x01,                     /* Usage Page (Generic Desktop) */
    0x09, 0x05,                     /* Usage (Game Pad) */
    0xA1, 0x01,                     /* Collection (Application) */
    0x09, 0x01,                     /*   Usage (Pointer) */
    0xA1, 0x00,                     /*   Collection (Physical): the left stick */
    0x09, 0x30,                     /*     Usage (X) */
    0x09, 0x31,                     /*     Usage (Y) */
    0x15, 0x00,                     /*     Logical Minimum (0) */
    0x27, 0xFF, 0xFF, 0x00, 0x00,   /*     Logical Maximum (65535) */
    0x75, 0x10,                     /*     Report Size (16) */
    0x95, 0x02,                     /*     Report Count (2) */
    0x81, 0x02,                     /*     Input (Data, Variable, Absolute) */
    0xC0,                           /*   End Collection */
    0x09, 0x01,                     /*   Usage (Pointer) */
    0xA1, 0x00,                     /*   Collection (Physical): the right stick */
    0x09, 0x33,                     /*     Usage (Rx) */
    0x09, 0x34,                     /*     Usage (Ry) */
    0x95, 0x02,                     /*     Report Count (2) */
    0x81, 0x02,                     /*     Input (Data, Variable, Absolute) */
    0xC0,                           /*   End Collection */
    0x09, 0x01,                     /*   Usage (Pointer) */
    0xA1, 0x00,                     /*   Collection (Physical): the triggers */
    0x09, 0x32,                     /*     Usage (Z) */
    0x95, 0x01,                     /*     Report Count (1) */
    0x81, 0x02,                     /*     Input (Data, Variable, Absolute) */
    0xC0,                           /*   End Collection */
    0x05, 0x09,                     /*   Usage Page (Button) */
    0x19, 0x01,                     /*   Usage Minimum (1) */
    0x29, 0x0A,                     /*   Usage Maximum (10) */
    0x25, 0x01,                     /*   Logical Maximum (1) */
    0x75, 0x01,                     /*   Report Size (1) */
    0x95, 0x0A,                     /*   Report Count (10) */
    0x81, 0x02,                     /*   Input (Data, Variable, Absolute) */
    0x05, 0x01,                     /*   Usage Page (Generic Desktop) */
    0x09, 0x39,                     /*   Usage (Hat switch) */
    0x15, 0x01,                     /*   Logical Minimum (1) */
    0x25, 0x08,                     /*   Logical Maximum (8) */
    0x35, 0x00,                     /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01,               /*   Physical Maximum (315) */
    0x65, 0x14,                     /*   Unit (English Rotation: degrees) */
    0x75, 0x04,                     /*   Report Size (4) */
    0x95, 0x01,                     /*   Report Count (1) */
    0x81, 0x42,                     /*   Input (Data, Variable, Absolute, Null State) */
    0x65, 0x00,                     /*   Unit (None) */
    0x45, 0x00,                     /*   Physical Maximum (0) */
    0x75, 0x01,                     /*   Report Size (1) */
    0x95, 0x12,                     /*   Report Count (18) */
    0x81, 0x03,                     /*   Input (Constant): pad to 14 bytes */
    0xC0,                           /* End Collection */
};
const size_t ios_hidpad_xinput_descriptor_len = sizeof(ios_hidpad_xinput_descriptor);

static const char *const slot_device_ids[IOS_HIDPAD_SLOTS] =
{
    "HID\\VID_045E&PID_028E&IG_00",
    "HID\\VID_045E&PID_028E&IG_01",
    "HID\\VID_045E&PID_028E&IG_02",
    "HID\\VID_045E&PID_028E&IG_03",
};

static const char *const slot_instances[IOS_HIDPAD_SLOTS] =
{
    "9&4d616465&1&0000",
    "9&4d616465&1&0001",
    "9&4d616465&1&0002",
    "9&4d616465&1&0003",
};

static const char *const slot_containers[IOS_HIDPAD_SLOTS] =
{
    "{6d796f73-6465-636b-0003-0000045e028e}",
    "{6d796f73-6465-636b-0003-0001045e028e}",
    "{6d796f73-6465-636b-0003-0002045e028e}",
    "{6d796f73-6465-636b-0003-0003045e028e}",
};

static void copy_string(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > IOS_HIDPAD_STRING_MAX) n = IOS_HIDPAD_STRING_MAX;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int ios_hidpad_identity_slot(unsigned int slot, struct ios_hidpad_identity *out)
{
    memset(out, 0, sizeof(*out));
    if (slot >= IOS_HIDPAD_SLOTS)
    {
        out->kind = IOS_HIDPAD_NONE;
        return 0;
    }
    out->kind = IOS_HIDPAD_XINPUT;
    out->name = "xinput";
    out->vendor_id = 0x045e;
    out->product_id = 0x028e;
    out->version = 0x0114;
    out->device_id = slot_device_ids[slot];
    out->instance = slot_instances[slot];
    out->container_id = slot_containers[slot];
    copy_string(out->product, IOS_HIDPAD_XINPUT_PRODUCT);
    copy_string(out->manufacturer, "Microsoft");
    out->serial[0] = 0;
    out->descriptor = ios_hidpad_xinput_descriptor;
    out->descriptor_len = sizeof(ios_hidpad_xinput_descriptor);
    out->input_report_len = IOS_HIDPAD_XINPUT_INPUT_LEN;
    out->slot = slot;
    return 1;
}

static void put16(uint8_t *p, unsigned int v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

size_t ios_hidpad_build_xinput_input(const struct winios_hidpad *pad, uint8_t *out)
{
    static const struct { uint32_t mask; unsigned int bit; } buttons[] =
    {
        { PAD_A, 0 }, { PAD_B, 1 }, { PAD_X, 2 }, { PAD_Y, 3 },
        { PAD_LEFT_SHOULDER, 4 }, { PAD_RIGHT_SHOULDER, 5 },
        { PAD_BACK, 6 }, { PAD_START, 7 },
        { PAD_LEFT_THUMB, 8 }, { PAD_RIGHT_THUMB, 9 },
    };
    struct winios_hidpad rest;
    unsigned int i, bits = 0;
    uint8_t hat;

    if (!pad->connected)
    {
        memset(&rest, 0, sizeof(rest));
        pad = &rest;
    }

    memset(out, 0, IOS_HIDPAD_XINPUT_INPUT_LEN);
    put16(out + 1, (unsigned int)((int)pad->lx + 32768));
    put16(out + 3, (unsigned int)(32767 - (int)pad->ly));
    put16(out + 5, (unsigned int)((int)pad->rx + 32768));
    put16(out + 7, (unsigned int)(32767 - (int)pad->ry));
    put16(out + 9, (unsigned int)(0x8000 + ((int)pad->left_trigger - (int)pad->right_trigger) * 128));

    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (pad->buttons & buttons[i].mask) bits |= 1u << buttons[i].bit;
    hat = ios_hidpad_hat_from_buttons(pad->buttons);
    if (hat != 8) bits |= (unsigned int)(hat + 1) << 10;
    put16(out + 11, bits);
    return IOS_HIDPAD_XINPUT_INPUT_LEN;
}

void ios_hidpad_from_gamepad(const struct winios_gamepad *gamepad, int connected,
                             struct winios_hidpad *out)
{
    memset(out, 0, sizeof(*out));
    out->packet = gamepad->packet;
    out->connected = (uint8_t)(connected ? 1 : 0);
    if (!connected) return;
    out->buttons = gamepad->buttons;
    out->lx = gamepad->lx;
    out->ly = gamepad->ly;
    out->rx = gamepad->rx;
    out->ry = gamepad->ry;
    out->left_trigger = gamepad->left_trigger;
    out->right_trigger = gamepad->right_trigger;
    out->battery = WINIOS_HIDPAD_BATTERY_UNKNOWN;
}

int ios_hidpad_slot_listed(unsigned int slot, int slot_connected, int hid_present)
{
    if (slot >= IOS_HIDPAD_SLOTS || !slot_connected) return 0;
    return !(slot == 0 && hid_present);
}

int ios_hidpad_rawinput_due(int *have, uint32_t *last_packet, int listed, uint32_t packet)
{
    if (!listed)
    {
        *have = 0;
        return 0;
    }
    if (*have && *last_packet == packet) return 0;
    *have = 1;
    *last_packet = packet;
    return 1;
}
