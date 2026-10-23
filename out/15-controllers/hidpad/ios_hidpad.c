// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part A: the HID pad's identity, names, report descriptors and reports.
// See ios_hidpad.h. Field layouts of the DualSense reports follow the USB report descriptor
// in refs/dualsense-public/report-descriptor-usb.txt and the structures described by SDL's
// PS5 driver (refs/dualsense-public/SDL_hidapi_ps5.c); the code is our own.

#include <string.h>

#include "ios_hidpad.h"

/* XINPUT_GAMEPAD_* bits (Microsoft's XInput documentation). */
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
#define PAD_GUIDE          0x0400u
#define PAD_A              0x1000u
#define PAD_B              0x2000u
#define PAD_X              0x4000u
#define PAD_Y              0x8000u

/* ---- A3: descriptors ------------------------------------------------------------------ */

/* The DualSense's USB report descriptor, byte for byte from
 * refs/dualsense-public/report-descriptor-usb.txt (a device dump, used as it is). */
const uint8_t ios_hidpad_dualsense_descriptor[IOS_HIDPAD_DUALSENSE_DESCRIPTOR_LEN] =
{
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02, 0x85, 0x02, 0x09, 0x23, 0x95, 0x2F, 0x91, 0x02,
    0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xB1, 0x02, 0x85, 0x08, 0x09, 0x34, 0x95, 0x2F, 0xB1, 0x02,
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xB1, 0x02, 0x85, 0x0A, 0x09, 0x25, 0x95, 0x1A, 0xB1, 0x02,
    0x85, 0x20, 0x09, 0x26, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xB1, 0x02,
    0x85, 0x22, 0x09, 0x40, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x80, 0x09, 0x28, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x81, 0x09, 0x29, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x82, 0x09, 0x2A, 0x95, 0x09, 0xB1, 0x02,
    0x85, 0x83, 0x09, 0x2B, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x84, 0x09, 0x2C, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x85, 0x09, 0x2D, 0x95, 0x02, 0xB1, 0x02, 0x85, 0xA0, 0x09, 0x2E, 0x95, 0x01, 0xB1, 0x02,
    0x85, 0xE0, 0x09, 0x2F, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF0, 0x09, 0x30, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF1, 0x09, 0x31, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF2, 0x09, 0x32, 0x95, 0x0F, 0xB1, 0x02,
    0x85, 0xF4, 0x09, 0x35, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF5, 0x09, 0x36, 0x95, 0x03, 0xB1, 0x02,
    0xC0,
};

/* Our own Game Pad descriptor (USB HID 1.11 items, HID Usage Tables pages 0x01 and 0x09).
 * Report 1, 14 bytes with the id:
 *   [1..2] X  [3..4] Y  [5..6] Rx  [7..8] Ry  [9..10] Z      16-bit signed, little endian
 *   [11]   hat in bits 0-3 (1-8 clockwise from north, 0 = centred: null state), bits 4-7 pad
 *   [12..13] buttons 1-11 in bits 0-10 (A B X Y LB RB Back Start L3 R3 Guide), bits 11-15 pad
 * The axes' logical range is -32767..32767 (symmetric, so the centre is exactly 0 for every
 * reader that scales around the middle of the range; see QUESTIONS.md, spec 15 item 3). */
const uint8_t ios_hidpad_generic_descriptor[] =
{
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x05,             /* Usage (Game Pad) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1) */
    0x09, 0x30,             /*   Usage (X) */
    0x09, 0x31,             /*   Usage (Y) */
    0x09, 0x33,             /*   Usage (Rx) */
    0x09, 0x34,             /*   Usage (Ry) */
    0x09, 0x32,             /*   Usage (Z) */
    0x16, 0x01, 0x80,       /*   Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F,       /*   Logical Maximum (32767) */
    0x75, 0x10,             /*   Report Size (16) */
    0x95, 0x05,             /*   Report Count (5) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x09, 0x39,             /*   Usage (Hat switch) */
    0x15, 0x01,             /*   Logical Minimum (1) */
    0x25, 0x08,             /*   Logical Maximum (8) */
    0x35, 0x00,             /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01,       /*   Physical Maximum (315) */
    0x65, 0x14,             /*   Unit (English Rotation: degrees) */
    0x75, 0x04,             /*   Report Size (4) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x42,             /*   Input (Data, Variable, Absolute, Null State) */
    0x65, 0x00,             /*   Unit (None) */
    0x45, 0x00,             /*   Physical Maximum (0) */
    0x75, 0x04,             /*   Report Size (4) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x03,             /*   Input (Constant): pad to the byte */
    0x05, 0x09,             /*   Usage Page (Button) */
    0x19, 0x01,             /*   Usage Minimum (1) */
    0x29, 0x0B,             /*   Usage Maximum (11) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x0B,             /*   Report Count (11) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x05,             /*   Report Count (5) */
    0x81, 0x03,             /*   Input (Constant): pad to 16 bits */
    0xC0,                   /* End Collection */
};
const size_t ios_hidpad_generic_descriptor_len = sizeof(ios_hidpad_generic_descriptor);

/* ---- A1: identities ------------------------------------------------------------------- */

static void copy_string(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > IOS_HIDPAD_STRING_MAX) n = IOS_HIDPAD_STRING_MAX;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* MADEIRA_HIDPAD_NAME, printable ASCII only: other bytes are dropped, surrounding spaces
 * trimmed. An empty result means "no usable name". */
static void copy_printable(char *dst, const char *src)
{
    size_t n = 0;

    for (; src && *src && n < IOS_HIDPAD_STRING_MAX; src++)
    {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20 || c > 0x7e) continue;
        if (c == ' ' && n == 0) continue;
        dst[n++] = (char)c;
    }
    while (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

int ios_hidpad_identity_get(enum ios_hidpad_kind kind, const char *name_env,
                            struct ios_hidpad_identity *out)
{
    memset(out, 0, sizeof(*out));

    switch (kind)
    {
    case IOS_HIDPAD_DUALSENSE:
        out->kind = IOS_HIDPAD_DUALSENSE;
        out->name = "dualsense";
        out->vendor_id = 0x054c;
        out->product_id = 0x0ce6;
        out->version = 0x0100;
        out->device_id = "HID\\VID_054C&PID_0CE6&MI_03";
        out->instance = "9&4d616465&0&0000";
        out->container_id = "{6d796f73-6465-636b-0001-0000054c0ce6}";
        copy_string(out->product, "DualSense Wireless Controller");
        copy_string(out->manufacturer, "Sony Interactive Entertainment");
        copy_string(out->serial, "02a14d616465");
        out->descriptor = ios_hidpad_dualsense_descriptor;
        out->descriptor_len = sizeof(ios_hidpad_dualsense_descriptor);
        out->input_report_len = IOS_HIDPAD_DUALSENSE_INPUT_LEN;
        return 1;

    case IOS_HIDPAD_GENERIC:
        out->kind = IOS_HIDPAD_GENERIC;
        out->name = "generic";
        out->vendor_id = 0x1209;
        out->product_id = 0x4d47;
        out->version = 0x0100;
        out->device_id = "HID\\VID_1209&PID_4D47";
        out->instance = "9&4d616465&0&0000";
        out->container_id = "{6d796f73-6465-636b-0002-000012094d47}";
        copy_printable(out->product, name_env);
        if (!out->product[0]) copy_string(out->product, "MYIOSDECK Gamepad");
        copy_string(out->manufacturer, "MYIOSDECK");
        out->serial[0] = 0;
        out->descriptor = ios_hidpad_generic_descriptor;
        out->descriptor_len = sizeof(ios_hidpad_generic_descriptor);
        out->input_report_len = IOS_HIDPAD_GENERIC_INPUT_LEN;
        return 1;

    case IOS_HIDPAD_XINPUT:
        return ios_hidpad_identity_slot(0, out);

    case IOS_HIDPAD_NONE:
        break;
    }

    out->kind = IOS_HIDPAD_NONE;
    return 0;
}

int ios_hidpad_identity_select(const char *hidpad_env, const char *name_env,
                               struct ios_hidpad_identity *out)
{
    enum ios_hidpad_kind kind = IOS_HIDPAD_NONE;

    if (hidpad_env && !strcmp(hidpad_env, "dualsense")) kind = IOS_HIDPAD_DUALSENSE;
    else if (hidpad_env && !strcmp(hidpad_env, "generic")) kind = IOS_HIDPAD_GENERIC;
    return ios_hidpad_identity_get(kind, name_env, out);
}

/* ---- A2: names ------------------------------------------------------------------------ */

struct sbuf
{
    char *buf;
    size_t size;
    size_t len;
    int overflow;
};

static void sb_init(struct sbuf *sb, char *buf, size_t size)
{
    sb->buf = buf;
    sb->size = size;
    sb->len = 0;
    sb->overflow = (size == 0);
    if (size) buf[0] = 0;
}

static void sb_add(struct sbuf *sb, const char *s, size_t n)
{
    if (sb->overflow) return;
    if (sb->len + n + 1 > sb->size)
    {
        sb->overflow = 1;
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = 0;
}

static void sb_str(struct sbuf *sb, const char *s)
{
    sb_add(sb, s, strlen(s));
}

/* Append `s` with every '\' turned into '#'. */
static void sb_hashed(struct sbuf *sb, const char *s)
{
    for (; *s; s++) sb_add(sb, *s == '\\' ? "#" : s, 1);
}

static void sb_hex4(struct sbuf *sb, uint16_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[4];
    tmp[0] = digits[(v >> 12) & 0xf];
    tmp[1] = digits[(v >> 8) & 0xf];
    tmp[2] = digits[(v >> 4) & 0xf];
    tmp[3] = digits[v & 0xf];
    sb_add(sb, tmp, 4);
}

static size_t sb_done(struct sbuf *sb)
{
    if (!sb->overflow) return sb->len;
    if (sb->size) sb->buf[0] = 0;
    return 0;
}

static void sb_link(struct sbuf *sb, const struct ios_hidpad_identity *id)
{
    sb_hashed(sb, id->device_id);
    sb_add(sb, "#", 1);
    sb_hashed(sb, id->instance);
    sb_add(sb, "#", 1);
    sb_str(sb, IOS_HIDPAD_HID_INTERFACE_GUID);
}

size_t ios_hidpad_link(const struct ios_hidpad_identity *id, char *buf, size_t size)
{
    struct sbuf sb;
    sb_init(&sb, buf, size);
    if (id->kind == IOS_HIDPAD_NONE) sb.overflow = 1;
    else sb_link(&sb, id);
    return sb_done(&sb);
}

static size_t prefixed_link(const struct ios_hidpad_identity *id, const char *prefix, char *buf, size_t size)
{
    struct sbuf sb;
    sb_init(&sb, buf, size);
    if (id->kind == IOS_HIDPAD_NONE) sb.overflow = 1;
    sb_str(&sb, prefix);
    sb_link(&sb, id);
    return sb_done(&sb);
}

size_t ios_hidpad_user_path(const struct ios_hidpad_identity *id, char *buf, size_t size)
{
    return prefixed_link(id, "\\\\?\\", buf, size);
}

size_t ios_hidpad_symlink_path(const struct ios_hidpad_identity *id, char *buf, size_t size)
{
    return prefixed_link(id, "\\??\\", buf, size);
}

size_t ios_hidpad_registry_link_key(const struct ios_hidpad_identity *id, char *buf, size_t size)
{
    return prefixed_link(id, "##?#", buf, size);
}

size_t ios_hidpad_instance_id(const struct ios_hidpad_identity *id, char *buf, size_t size)
{
    struct sbuf sb;
    sb_init(&sb, buf, size);
    if (id->kind == IOS_HIDPAD_NONE) sb.overflow = 1;
    sb_str(&sb, id->device_id);
    sb_add(&sb, "\\", 1);
    sb_str(&sb, id->instance);
    return sb_done(&sb);
}

size_t ios_hidpad_hardware_id(const struct ios_hidpad_identity *id, unsigned int index,
                              char *buf, size_t size)
{
    struct sbuf sb;
    const char *mi;

    sb_init(&sb, buf, size);
    if (id->kind == IOS_HIDPAD_NONE) sb.overflow = 1;

    switch (index)
    {
    case 0:   /* the device id with &REV_xxxx before any &MI_ (or an XInput pad's &IG_) */
        mi = strstr(id->device_id, "&MI_");
        if (!mi) mi = strstr(id->device_id, "&IG_");
        if (mi)
        {
            sb_add(&sb, id->device_id, (size_t)(mi - id->device_id));
            sb_str(&sb, "&REV_");
            sb_hex4(&sb, id->version);
            sb_str(&sb, mi);
        }
        else
        {
            sb_str(&sb, id->device_id);
            sb_str(&sb, "&REV_");
            sb_hex4(&sb, id->version);
        }
        break;
    case 1:
        sb_str(&sb, id->device_id);
        break;
    case 2:
        sb_str(&sb, "HID\\VID_");
        sb_hex4(&sb, id->vendor_id);
        sb_str(&sb, "&UP:0001_U:0005");
        break;
    case 3:
        sb_str(&sb, "HID_DEVICE_SYSTEM_GAME");
        break;
    case 4:
        sb_str(&sb, "HID_DEVICE_UP:0001_U:0005");
        break;
    case 5:
        sb_str(&sb, "HID_DEVICE");
        break;
    default:
        sb.overflow = 1;
        break;
    }
    return sb_done(&sb);
}

/* ---- A4: input reports ---------------------------------------------------------------- */

static void put16(uint8_t *p, int v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint8_t ios_hidpad_hat_from_buttons(uint32_t buttons)
{
    /* index [vertical + 1][horizontal + 1]; vertical +1 = up, horizontal +1 = right */
    static const uint8_t table[3][3] =
    {
        { 5, 4, 3 },    /* down:   SW S SE */
        { 6, 8, 2 },    /* middle: W  -  E */
        { 7, 0, 1 },    /* up:     NW N NE */
    };
    int v = !!(buttons & PAD_DPAD_UP) - !!(buttons & PAD_DPAD_DOWN);
    int h = !!(buttons & PAD_DPAD_RIGHT) - !!(buttons & PAD_DPAD_LEFT);
    return table[v + 1][h + 1];
}

int16_t ios_hidpad_flip_y(int16_t v)
{
    if (v == -32768) return 32767;
    return (int16_t)-v;
}

/* Full signed 16-bit to the DualSense's 0..255 (centre 128). */
static uint8_t stick_byte(int16_t v)
{
    return (uint8_t)(((int32_t)v + 32768) >> 8);
}

/* The DualSense battery byte: level 0-10 in the low nibble, the charging state above. */
static uint8_t battery_byte(const struct winios_hidpad *pad)
{
    unsigned int level;

    if (!pad->connected || pad->battery == WINIOS_HIDPAD_BATTERY_UNKNOWN) return 0x2a;
    level = pad->battery >= 100 ? 10u : pad->battery / 10u;
    return (uint8_t)(((pad->charging & 0x0f) << 4) | level);
}

static void put_touch(uint8_t *p, const struct winios_hidpad *pad,
                      struct ios_hidpad_report_state *state, int i)
{
    int down = pad->connected && pad->touch[i];
    unsigned int x = pad->touch_x[i], y = pad->touch_y[i];

    if (down && !state->touch_down[i])
    {
        state->touch_id[i] = state->next_touch_id & 0x7f;
        state->next_touch_id = (uint8_t)((state->next_touch_id + 1) & 0x7f);
    }
    state->touch_down[i] = (uint8_t)down;

    if (!down) x = y = 0;
    if (x > 1919) x = 1919;
    if (y > 1079) y = 1079;

    p[0] = (uint8_t)((state->touch_id[i] & 0x7f) | (down ? 0 : 0x80));
    p[1] = (uint8_t)(x & 0xff);
    p[2] = (uint8_t)(((x >> 8) & 0x0f) | ((y & 0x0f) << 4));
    p[3] = (uint8_t)(y >> 4);
}

size_t ios_hidpad_build_dualsense_input(const struct winios_hidpad *pad,
                                        struct ios_hidpad_report_state *state,
                                        uint64_t now_us, uint8_t *out)
{
    struct winios_hidpad rest;
    uint32_t b;
    uint8_t byte;
    int i;

    if (!pad->connected)
    {
        /* a pad at rest: everything neutral, battery unknown */
        memset(&rest, 0, sizeof(rest));
        rest.packet = pad->packet;
        pad = &rest;
    }
    b = pad->connected ? pad->buttons : 0;

    memset(out, 0, IOS_HIDPAD_DUALSENSE_INPUT_LEN);
    out[0] = 0x01;
    out[1] = stick_byte(pad->lx);
    out[2] = stick_byte(ios_hidpad_flip_y(pad->ly));
    out[3] = stick_byte(pad->rx);
    out[4] = stick_byte(ios_hidpad_flip_y(pad->ry));
    out[5] = pad->left_trigger;
    out[6] = pad->right_trigger;
    out[7] = state->counter++;

    byte = ios_hidpad_hat_from_buttons(b);
    if (b & PAD_X) byte |= 0x10;               /* square */
    if (b & PAD_A) byte |= 0x20;               /* cross */
    if (b & PAD_B) byte |= 0x40;               /* circle */
    if (b & PAD_Y) byte |= 0x80;               /* triangle */
    out[8] = byte;

    byte = 0;
    if (b & PAD_LEFT_SHOULDER) byte |= 0x01;
    if (b & PAD_RIGHT_SHOULDER) byte |= 0x02;
    if (b & WINIOS_HIDPAD_L2) byte |= 0x04;
    if (b & WINIOS_HIDPAD_R2) byte |= 0x08;
    if (b & PAD_BACK) byte |= 0x10;            /* Create */
    if (b & PAD_START) byte |= 0x20;           /* Options */
    if (b & PAD_LEFT_THUMB) byte |= 0x40;
    if (b & PAD_RIGHT_THUMB) byte |= 0x80;
    out[9] = byte;

    byte = 0;
    if (b & PAD_GUIDE) byte |= 0x01;           /* PS */
    if (b & WINIOS_HIDPAD_TOUCHPAD) byte |= 0x02;
    if (b & WINIOS_HIDPAD_MUTE) byte |= 0x04;
    out[10] = byte;

    if (pad->connected && pad->motion == 1)
    {
        for (i = 0; i < 3; i++)
        {
            put16(out + 16 + 2 * i, pad->gyro[i]);
            put16(out + 22 + 2 * i, pad->accel[i]);
        }
    }
    else
    {
        put16(out + 24, 8192);                 /* lying flat: 1 g on Y */
    }
    put32(out + 28, (uint32_t)(now_us * 3u));

    put_touch(out + 33, pad, state, 0);
    put_touch(out + 37, pad, state, 1);

    out[53] = battery_byte(pad);
    return IOS_HIDPAD_DUALSENSE_INPUT_LEN;
}

/* Signed axis for the generic report: the logical range is -32767..32767. */
static int16_t sym(int16_t v)
{
    return v == -32768 ? (int16_t)-32767 : v;
}

size_t ios_hidpad_build_generic_input(const struct winios_hidpad *pad, uint8_t *out)
{
    static const struct { uint32_t mask; unsigned int bit; } buttons[] =
    {
        { PAD_A, 0 }, { PAD_B, 1 }, { PAD_X, 2 }, { PAD_Y, 3 },
        { PAD_LEFT_SHOULDER, 4 }, { PAD_RIGHT_SHOULDER, 5 },
        { PAD_BACK, 6 }, { PAD_START, 7 },
        { PAD_LEFT_THUMB, 8 }, { PAD_RIGHT_THUMB, 9 }, { PAD_GUIDE, 10 },
    };
    unsigned int i, bits = 0;
    uint8_t hat;
    int z;

    memset(out, 0, IOS_HIDPAD_GENERIC_INPUT_LEN);
    out[0] = 0x01;
    if (!pad->connected) return IOS_HIDPAD_GENERIC_INPUT_LEN;   /* at rest: all zero, hat centred */

    put16(out + 1, sym(pad->lx));
    put16(out + 3, ios_hidpad_flip_y(pad->ly));
    put16(out + 5, sym(pad->rx));
    put16(out + 7, ios_hidpad_flip_y(pad->ry));
    z = ((int)pad->left_trigger - (int)pad->right_trigger) * 32767 / 255;
    put16(out + 9, z);

    hat = ios_hidpad_hat_from_buttons(pad->buttons);
    out[11] = (uint8_t)(hat == 8 ? 0 : hat + 1);

    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (pad->buttons & buttons[i].mask) bits |= 1u << buttons[i].bit;
    put16(out + 12, (int)bits);
    return IOS_HIDPAD_GENERIC_INPUT_LEN;
}

size_t ios_hidpad_build_input(const struct ios_hidpad_identity *id, const struct winios_hidpad *pad,
                              struct ios_hidpad_report_state *state, uint64_t now_us, uint8_t *out)
{
    switch (id->kind)
    {
    case IOS_HIDPAD_DUALSENSE: return ios_hidpad_build_dualsense_input(pad, state, now_us, out);
    case IOS_HIDPAD_GENERIC: return ios_hidpad_build_generic_input(pad, out);
    case IOS_HIDPAD_XINPUT: return ios_hidpad_build_xinput_input(pad, out);
    case IOS_HIDPAD_NONE: break;
    }
    return 0;
}

/* ---- A5: feature reports -------------------------------------------------------------- */

#define GYRO_FULL   8640    /* = 540 deg/s, the store's units */
#define GYRO_SPEED  540
#define ACCEL_1G    8192

void ios_hidpad_build_feature(const struct ios_hidpad_identity *id, uint8_t report_id,
                              uint8_t *out, size_t len)
{
    static const char build_date[] = "Oct 19 2026";   /* 11 characters */
    static const char build_time[] = "10:00:00";      /* 8 characters */
    /* 02:a1:4d:61:64:65, stored with its last byte first */
    static const uint8_t address_reversed[6] = { 0x65, 0x64, 0x61, 0x4d, 0xa1, 0x02 };
    uint8_t tmp[64];
    size_t n;
    int i;

    if (!len) return;
    memset(out, 0, len);
    memset(tmp, 0, sizeof(tmp));
    tmp[0] = report_id;

    if (id->kind == IOS_HIDPAD_DUALSENSE)
    {
        switch (report_id)
        {
        case 0x05:  /* calibration: zero bias, the store's units */
            /* 1..6: gyro pitch/yaw/roll bias = 0 */
            for (i = 0; i < 3; i++)
            {
                put16(tmp + 7 + 4 * i, GYRO_FULL);     /* plus */
                put16(tmp + 9 + 4 * i, -GYRO_FULL);    /* minus */
            }
            put16(tmp + 19, GYRO_SPEED);
            put16(tmp + 21, GYRO_SPEED);
            for (i = 0; i < 3; i++)
            {
                put16(tmp + 23 + 4 * i, ACCEL_1G);
                put16(tmp + 25 + 4 * i, -ACCEL_1G);
            }
            break;
        case 0x09:  /* pairing info */
            memcpy(tmp + 1, address_reversed, sizeof(address_reversed));
            break;
        case 0x20:  /* firmware info */
            memcpy(tmp + 1, build_date, 11);
            memcpy(tmp + 12, build_time, 8);
            tmp[44] = 0x24;
            tmp[45] = 0x02;
            break;
        default:
            break;
        }
    }

    n = len < sizeof(tmp) ? len : sizeof(tmp);
    memcpy(out, tmp, n);
}

/* ---- A6: output reports --------------------------------------------------------------- */

uint32_t ios_hidpad_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    size_t i;
    int k;

    crc = ~crc;
    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Offsets in the common effects block (USB report 0x02 from byte 1, Bluetooth 0x31 from 3). */
enum
{
    FX_FLAGS0 = 0,          /* 0x01 compatible vibration, 0x02 haptics select,
                               0x04 right trigger effect, 0x08 left trigger effect */
    FX_FLAGS1 = 1,          /* 0x01 mic LED, 0x04 light bar, 0x10 player LEDs,
                               0x40 motor power reduction */
    FX_RUMBLE_RIGHT = 2,    /* small motor */
    FX_RUMBLE_LEFT = 3,     /* large motor */
    FX_MIC_LED = 8,
    FX_RIGHT_TRIGGER = 10,  /* 11 bytes */
    FX_LEFT_TRIGGER = 21,   /* 11 bytes */
    FX_POWER = 36,          /* two 3-bit reductions, one per nibble */
    FX_FLAGS2 = 38,         /* 0x04 improved rumble */
    FX_PLAYER_LEDS = 43,
    FX_RED = 44,
    FX_GREEN = 45,
    FX_BLUE = 46,
    FX_LEN = 47,
};

enum ios_hidpad_output_result ios_hidpad_decode_output(const uint8_t *data, size_t len,
                                                       struct winios_hidpad_output *out,
                                                       struct ios_hidpad_output_flags *flags)
{
    struct ios_hidpad_output_flags f;
    const uint8_t *c;
    uint8_t hdr = 0xa2;
    uint32_t crc, want;

    memset(&f, 0, sizeof(f));
    if (flags) *flags = f;
    if (!data || !len) return IOS_HIDPAD_OUTPUT_IGNORED;

    if (data[0] == 0x02 && len >= IOS_HIDPAD_DUALSENSE_USB_OUTPUT_LEN)
        c = data + 1;
    else if (data[0] == 0x31 && len >= IOS_HIDPAD_DUALSENSE_BT_OUTPUT_LEN)
    {
        crc = ios_hidpad_crc32(0, &hdr, 1);
        crc = ios_hidpad_crc32(crc, data, IOS_HIDPAD_DUALSENSE_BT_OUTPUT_LEN - 4);
        want = (uint32_t)data[74] | ((uint32_t)data[75] << 8) |
               ((uint32_t)data[76] << 16) | ((uint32_t)data[77] << 24);
        if (crc != want) return IOS_HIDPAD_OUTPUT_BAD_CRC;
        c = data + 3;
    }
    else return IOS_HIDPAD_OUTPUT_IGNORED;

    if ((c[FX_FLAGS0] & 0x03) || (c[FX_FLAGS2] & 0x04))
    {
        out->rumble_valid = 1;
        out->rumble_right = c[FX_RUMBLE_RIGHT];
        out->rumble_left = c[FX_RUMBLE_LEFT];
        f.rumble = 1;
    }
    if (c[FX_FLAGS0] & 0x04)
    {
        out->trigger_valid[1] = 1;
        memcpy(out->trigger[1], c + FX_RIGHT_TRIGGER, 11);
        f.triggers = 1;
    }
    if (c[FX_FLAGS0] & 0x08)
    {
        out->trigger_valid[0] = 1;
        memcpy(out->trigger[0], c + FX_LEFT_TRIGGER, 11);
        f.triggers = 1;
    }
    if (c[FX_FLAGS1] & 0x01)
    {
        out->mute_led_valid = 1;
        out->mute_led = c[FX_MIC_LED];
        f.leds = 1;
    }
    if (c[FX_FLAGS1] & 0x40)
    {
        out->power_valid = 1;
        out->power_reduction = c[FX_POWER] & 0x77;
        f.power = 1;
    }
    if (c[FX_FLAGS1] & 0x10)
    {
        out->player_leds_valid = 1;
        out->player_leds = c[FX_PLAYER_LEDS];
        f.leds = 1;
    }
    if (c[FX_FLAGS1] & 0x04)
    {
        out->lightbar_valid = 1;
        out->red = c[FX_RED];
        out->green = c[FX_GREEN];
        out->blue = c[FX_BLUE];
        f.lightbar = 1;
    }

    if (flags) *flags = f;
    return IOS_HIDPAD_OUTPUT_FOLDED;
}

int ios_hidpad_output_release(struct winios_hidpad_output *out)
{
    struct winios_hidpad_output before = *out;

    if (out->rumble_valid)
    {
        out->rumble_left = 0;
        out->rumble_right = 0;
    }
    if (out->trigger_valid[0] || out->trigger_valid[1])
    {
        out->trigger_valid[0] = 1;
        out->trigger_valid[1] = 1;
        memset(out->trigger, 0, sizeof(out->trigger));
    }
    return !ios_hidpad_output_same(&before, out);
}

int ios_hidpad_output_same(const struct winios_hidpad_output *a, const struct winios_hidpad_output *b)
{
    struct winios_hidpad_output x = *a, y = *b;
    x.serial = y.serial = 0;
    return !memcmp(&x, &y, sizeof(x));
}

/* ---- B3 / B7 helpers ------------------------------------------------------------------ */

int ios_hidpad_read_ready(uint64_t now_us, int have_last, uint64_t last_report_us,
                          uint32_t last_packet, uint32_t packet)
{
    uint64_t gap;

    if (!have_last) return 1;
    gap = now_us >= last_report_us ? now_us - last_report_us : 0;
    if (gap >= IOS_HIDPAD_TICK_US) return 1;
    if (packet != last_packet && gap >= IOS_HIDPAD_FRESH_US) return 1;
    return 0;
}

int ios_hidpad_effects_log_kind(uint32_t count)
{
    if (count >= 1 && count <= 8) return 1;
    if (count == 100 || count == 1000 || count == 10000) return 2;
    return 0;
}

/* ---- clarification 4: the gate -------------------------------------------------------- */

int ios_hidpad_open_allowed(int connected, uint32_t *refusals, int *log_refusal)
{
    if (log_refusal) *log_refusal = 0;
    if (connected) return 1;
    if (*refusals < UINT32_MAX) (*refusals)++;
    if (log_refusal && *refusals <= IOS_HIDPAD_REFUSALS_LOGGED) *log_refusal = 1;
    return 0;
}
