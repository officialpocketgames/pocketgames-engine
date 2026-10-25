// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part F: host tests of Part A (identities, names, descriptors, input / feature /
// output reports) and of the pure helpers of Part B (read pacing, effects log).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ios_hidpad.h"
#include "test_common.h"

#define DPAD_UP 0x0001u
#define DPAD_DOWN 0x0002u
#define DPAD_LEFT 0x0004u
#define DPAD_RIGHT 0x0008u
#define BTN_START 0x0010u
#define BTN_BACK 0x0020u
#define BTN_LB 0x0100u
#define BTN_GUIDE 0x0400u
#define BTN_A 0x1000u
#define BTN_Y 0x8000u

static int le16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static unsigned int ule16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* ---- a minimal walker of HID short items: report lengths per id, with the id byte ---- */

struct report_lengths { unsigned int input[256], output[256], feature[256]; };

static void walk_descriptor(const uint8_t *d, size_t len, struct report_lengths *r)
{
    unsigned int size = 0, count = 0, id = 0;
    size_t i = 0;

    memset(r, 0, sizeof(*r));
    while (i < len)
    {
        uint8_t prefix = d[i];
        unsigned int n = prefix & 3, type = (prefix >> 2) & 3, tag = prefix >> 4, value = 0, k;
        if (n == 3) n = 4;
        for (k = 0; k < n; k++) value |= (unsigned int)d[i + 1 + k] << (8 * k);
        i += 1 + n;
        if (type == 1)          /* global */
        {
            if (tag == 7) size = value;
            if (tag == 8) id = value;
            if (tag == 9) count = value;
        }
        else if (type == 0)     /* main */
        {
            if (tag == 8) r->input[id] += size * count;
            if (tag == 9) r->output[id] += size * count;
            if (tag == 11) r->feature[id] += size * count;
        }
    }
}

static unsigned int bytes_with_id(unsigned int bits) { return bits ? (bits + 7) / 8 + 1 : 0; }

/* ---- A1 / A2 ---------------------------------------------------------------------------- */

static void test_identities(void)
{
    struct ios_hidpad_identity id;
    char buf[256];

    CHECK_EQ(ios_hidpad_identity_select("dualsense", "ignored", &id), 1);
    CHECK_EQ(id.kind, IOS_HIDPAD_DUALSENSE);
    CHECK_EQ(id.vendor_id, 0x054c);
    CHECK_EQ(id.product_id, 0x0ce6);
    CHECK_EQ(id.version, 0x0100);
    CHECK_STR(id.device_id, "HID\\VID_054C&PID_0CE6&MI_03");
    CHECK_STR(id.instance, "9&4d616465&0&0000");
    CHECK_STR(id.product, "DualSense Wireless Controller");
    CHECK_STR(id.manufacturer, "Sony Interactive Entertainment");
    CHECK_STR(id.serial, "02a14d616465");
    CHECK_EQ(id.descriptor_len, 273);
    CHECK_EQ(id.input_report_len, 64);
    CHECK(id.container_id && strlen(id.container_id) == 38);

    CHECK_EQ(ios_hidpad_identity_select("generic", NULL, &id), 1);
    CHECK_EQ(id.kind, IOS_HIDPAD_GENERIC);
    CHECK_EQ(id.vendor_id, 0x1209);
    CHECK_EQ(id.product_id, 0x4d47);
    CHECK_STR(id.device_id, "HID\\VID_1209&PID_4D47");
    CHECK_STR(id.product, "MYIOSDECK Gamepad");
    CHECK_STR(id.manufacturer, "MYIOSDECK");
    CHECK_STR(id.serial, "");
    CHECK_EQ(id.input_report_len, 14);

    ios_hidpad_identity_select("generic", "  Xbox\x01 Wireless\tController\xc3\xa9 ", &id);
    CHECK_STR(id.product, "Xbox WirelessController");
    ios_hidpad_identity_select("generic", "\x80\x81\x7f", &id);
    CHECK_STR(id.product, "MYIOSDECK Gamepad");
    ios_hidpad_identity_select("generic", "", &id);
    CHECK_STR(id.product, "MYIOSDECK Gamepad");

    CHECK_EQ(ios_hidpad_identity_select(NULL, NULL, &id), 0);
    CHECK_EQ(id.kind, IOS_HIDPAD_NONE);
    CHECK_EQ(ios_hidpad_identity_select("DualSense", NULL, &id), 0);
    CHECK_EQ(ios_hidpad_identity_select("1", NULL, &id), 0);
    CHECK_EQ(ios_hidpad_identity_select("", NULL, &id), 0);
    CHECK_EQ(ios_hidpad_link(&id, buf, sizeof(buf)), 0);

    /* names, DualSense */
    ios_hidpad_identity_select("dualsense", NULL, &id);
    CHECK(ios_hidpad_link(&id, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "HID#VID_054C&PID_0CE6&MI_03#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    CHECK_EQ(ios_hidpad_link(&id, buf, sizeof(buf)), strlen(buf));
    ios_hidpad_user_path(&id, buf, sizeof(buf));
    CHECK_STR(buf, "\\\\?\\HID#VID_054C&PID_0CE6&MI_03#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    ios_hidpad_symlink_path(&id, buf, sizeof(buf));
    CHECK_STR(buf, "\\??\\HID#VID_054C&PID_0CE6&MI_03#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    ios_hidpad_registry_link_key(&id, buf, sizeof(buf));
    CHECK_STR(buf, "##?#HID#VID_054C&PID_0CE6&MI_03#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    ios_hidpad_instance_id(&id, buf, sizeof(buf));
    CHECK_STR(buf, "HID\\VID_054C&PID_0CE6&MI_03\\9&4d616465&0&0000");
    ios_hidpad_hardware_id(&id, 0, buf, sizeof(buf));
    CHECK_STR(buf, "HID\\VID_054C&PID_0CE6&REV_0100&MI_03");
    ios_hidpad_hardware_id(&id, 1, buf, sizeof(buf));
    CHECK_STR(buf, "HID\\VID_054C&PID_0CE6&MI_03");
    ios_hidpad_hardware_id(&id, 2, buf, sizeof(buf));
    CHECK_STR(buf, "HID\\VID_054C&UP:0001_U:0005");
    ios_hidpad_hardware_id(&id, 3, buf, sizeof(buf));
    CHECK_STR(buf, "HID_DEVICE_SYSTEM_GAME");
    ios_hidpad_hardware_id(&id, 4, buf, sizeof(buf));
    CHECK_STR(buf, "HID_DEVICE_UP:0001_U:0005");
    ios_hidpad_hardware_id(&id, 5, buf, sizeof(buf));
    CHECK_STR(buf, "HID_DEVICE");
    CHECK_EQ(ios_hidpad_hardware_id(&id, 6, buf, sizeof(buf)), 0);
    CHECK_STR(buf, "");

    /* too small: nothing, empty string */
    CHECK_EQ(ios_hidpad_link(&id, buf, 10), 0);
    CHECK_STR(buf, "");
    CHECK_EQ(ios_hidpad_link(&id, buf, 84), 0);   /* exactly the length: no room for the NUL */
    CHECK_EQ(ios_hidpad_link(&id, buf, 85), 84);

    /* names, generic */
    ios_hidpad_identity_select("generic", NULL, &id);
    ios_hidpad_link(&id, buf, sizeof(buf));
    CHECK_STR(buf, "HID#VID_1209&PID_4D47#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    ios_hidpad_registry_link_key(&id, buf, sizeof(buf));
    CHECK_STR(buf, "##?#HID#VID_1209&PID_4D47#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    ios_hidpad_hardware_id(&id, 0, buf, sizeof(buf));
    CHECK_STR(buf, "HID\\VID_1209&PID_4D47&REV_0100");
    ios_hidpad_hardware_id(&id, 2, buf, sizeof(buf));
    CHECK_STR(buf, "HID\\VID_1209&UP:0001_U:0005");
    CHECK_STR(IOS_HIDPAD_DEVICE_OBJECT, "\\Device\\MyiosdeckHidPad0");
}

/* ---- A3 ---------------------------------------------------------------------------------- */

static void test_descriptors(void)
{
    struct report_lengths r;

    CHECK_EQ(sizeof(ios_hidpad_dualsense_descriptor), 273);
    CHECK_EQ(ios_hidpad_dualsense_descriptor[0], 0x05);
    CHECK_EQ(ios_hidpad_dualsense_descriptor[3], 0x05);   /* Game Pad */
    CHECK_EQ(ios_hidpad_dualsense_descriptor[272], 0xc0);
    walk_descriptor(ios_hidpad_dualsense_descriptor, 273, &r);
    CHECK_EQ(bytes_with_id(r.input[1]), 64);
    CHECK_EQ(bytes_with_id(r.output[2]), 48);
    CHECK_EQ(bytes_with_id(r.feature[0x05]), 41);
    CHECK_EQ(bytes_with_id(r.feature[0x09]), 20);
    CHECK_EQ(bytes_with_id(r.feature[0x20]), 64);
    CHECK_EQ(r.output[0x31], 0);   /* the USB descriptor has no Bluetooth report */

    walk_descriptor(ios_hidpad_generic_descriptor, ios_hidpad_generic_descriptor_len, &r);
    CHECK_EQ(bytes_with_id(r.input[1]), 14);
    CHECK_EQ(r.output[1], 0);
    CHECK_EQ(r.feature[1], 0);
    CHECK_EQ(ios_hidpad_generic_descriptor[ios_hidpad_generic_descriptor_len - 1], 0xc0);
}

/* ---- A4 ---------------------------------------------------------------------------------- */

static void test_hat(void)
{
    CHECK_EQ(ios_hidpad_hat_from_buttons(0), 8);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_UP), 0);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_UP | DPAD_RIGHT), 1);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_RIGHT), 2);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_DOWN | DPAD_RIGHT), 3);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_DOWN), 4);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_DOWN | DPAD_LEFT), 5);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_LEFT), 6);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_UP | DPAD_LEFT), 7);
    /* opposite directions cancel */
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_UP | DPAD_DOWN), 8);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_LEFT | DPAD_RIGHT), 8);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_UP | DPAD_DOWN | DPAD_LEFT), 6);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_UP | DPAD_LEFT | DPAD_RIGHT), 0);
    CHECK_EQ(ios_hidpad_hat_from_buttons(DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT), 8);
    CHECK_EQ(ios_hidpad_hat_from_buttons(BTN_A | BTN_START), 8);

    CHECK_EQ(ios_hidpad_flip_y(-32768), 32767);
    CHECK_EQ(ios_hidpad_flip_y(32767), -32767);
    CHECK_EQ(ios_hidpad_flip_y(0), 0);
    CHECK_EQ(ios_hidpad_flip_y(1), -1);
}

static void test_dualsense_input(void)
{
    struct ios_hidpad_identity id;
    struct ios_hidpad_report_state st;
    struct pgios_hidpad pad;
    uint8_t r[64];
    int i;

    ios_hidpad_identity_select("dualsense", NULL, &id);
    memset(&st, 0, sizeof(st));
    memset(&pad, 0, sizeof(pad));
    pad.packet = 7;
    pad.connected = 1;
    pad.buttons = BTN_A | DPAD_UP | DPAD_RIGHT | BTN_LB | BTN_START | BTN_GUIDE |
                  PGIOS_HIDPAD_TOUCHPAD | PGIOS_HIDPAD_L2;
    pad.lx = -32768;
    pad.ly = -32768;    /* fully down */
    pad.rx = 32767;
    pad.ry = 0;
    pad.left_trigger = 200;
    pad.right_trigger = 10;
    pad.battery = 57;
    pad.charging = 1;
    pad.touch[0] = 1;
    pad.touch_x[0] = 1919;
    pad.touch_y[0] = 1079;

    CHECK_EQ(ios_hidpad_build_input(&id, &pad, &st, 1000, r), 64);
    CHECK_EQ(r[0], 0x01);
    CHECK_EQ(r[1], 0);       /* left stick full left */
    CHECK_EQ(r[2], 255);     /* store's -32768 (down) is HID's full down */
    CHECK_EQ(r[3], 255);
    CHECK_EQ(r[4], 128);     /* centred */
    CHECK_EQ(r[5], 200);
    CHECK_EQ(r[6], 10);
    CHECK_EQ(r[7], 0);       /* counter */
    CHECK_EQ(r[8], 0x21);    /* hat NE, cross */
    CHECK_EQ(r[9], 0x25);    /* L1, L2, Options */
    CHECK_EQ(r[10], 0x03);   /* PS, touchpad */
    CHECK_EQ(r[11], 0);
    for (i = 16; i < 22; i++) CHECK_EQ(r[i], 0);   /* gyro at rest */
    CHECK_EQ(le16(r + 22), 0);
    CHECK_EQ(le16(r + 24), 8192);                    /* 1 g on Y */
    CHECK_EQ(le16(r + 26), 0);
    CHECK_EQ(r[28], 0xb8);                           /* 1000 us x 3 = 3000 */
    CHECK_EQ(r[29], 0x0b);
    CHECK_EQ(r[30], 0);
    CHECK_EQ(r[31], 0);
    CHECK_EQ(r[33], 0x00);                           /* finger id 0, touching */
    CHECK_EQ(r[34], 0x7f);                           /* x = 1919 = 0x77f */
    CHECK_EQ(r[35], 0x77);                           /* x high nibble 7, y low nibble 7 */
    CHECK_EQ(r[36], 0x43);                           /* y = 1079 = 0x437 */
    CHECK_EQ(r[37], 0x80);                           /* second point not touching */
    CHECK_EQ(r[38], 0);
    CHECK_EQ(r[53], 0x15);                           /* charging, level 5 */

    /* next report: counter moves, same finger keeps its id */
    pad.touch_x[0] = 5000;                           /* clamped */
    pad.touch_y[0] = 2;
    ios_hidpad_build_input(&id, &pad, &st, 2000, r);
    CHECK_EQ(r[7], 1);
    CHECK_EQ(r[33], 0x00);
    CHECK_EQ(r[34], 0x7f);
    CHECK_EQ(r[35], 0x27);
    CHECK_EQ(r[36], 0x00);
    CHECK_EQ(ule16(r + 28) | (r[30] << 16), 6000);

    /* finger up: bit 7 set, id kept; down again: a new id; second finger: the next one */
    pad.touch[0] = 0;
    ios_hidpad_build_input(&id, &pad, &st, 3000, r);
    CHECK_EQ(r[33], 0x80);
    CHECK_EQ(r[34], 0);
    pad.touch[0] = 1;
    pad.touch[1] = 1;
    pad.touch_x[1] = 960;
    pad.touch_y[1] = 540;
    ios_hidpad_build_input(&id, &pad, &st, 4000, r);
    CHECK_EQ(r[33], 0x01);
    CHECK_EQ(r[37], 0x02);
    CHECK_EQ(r[38], 960 & 0xff);
    CHECK_EQ(r[39], ((960 >> 8) & 0x0f) | ((540 & 0x0f) << 4));
    CHECK_EQ(r[40], 540 >> 4);

    /* ids wrap at 7 bits */
    st.next_touch_id = 0x7f;
    pad.touch[0] = 0;
    ios_hidpad_build_input(&id, &pad, &st, 5000, r);
    pad.touch[0] = 1;
    ios_hidpad_build_input(&id, &pad, &st, 6000, r);
    CHECK_EQ(r[33], 0x7f);
    CHECK_EQ(st.next_touch_id, 0);

    /* counter wraps */
    st.counter = 255;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[7], 255);
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[7], 0);

    /* every face, shoulder and stick button */
    pad.buttons = 0x4000 /* X */ | 0x2000 /* B */ | BTN_Y | 0x0200 /* RB */ | BTN_BACK |
                  0x0040 | 0x0080 | PGIOS_HIDPAD_R2 | PGIOS_HIDPAD_MUTE | DPAD_LEFT | DPAD_RIGHT;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[8], 0x08 | 0x10 | 0x40 | 0x80);       /* hat centred (cancelled), square, circle, triangle */
    CHECK_EQ(r[9], 0x02 | 0x08 | 0x10 | 0x40 | 0x80);
    CHECK_EQ(r[10], 0x04);

    /* motion from the store */
    pad.motion = 1;
    pad.gyro[0] = 1; pad.gyro[1] = -2; pad.gyro[2] = 3;
    pad.accel[0] = -4; pad.accel[1] = 5; pad.accel[2] = -6;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(le16(r + 16), 1);
    CHECK_EQ(le16(r + 18), -2);
    CHECK_EQ(le16(r + 20), 3);
    CHECK_EQ(le16(r + 22), -4);
    CHECK_EQ(le16(r + 24), 5);
    CHECK_EQ(le16(r + 26), -6);
    pad.motion = 2;                                    /* only 1 means live */
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(le16(r + 16), 0);
    CHECK_EQ(le16(r + 24), 8192);

    /* battery byte */
    pad.battery = PGIOS_HIDPAD_BATTERY_UNKNOWN; pad.charging = 0;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[53], 0x2a);
    pad.battery = 100; pad.charging = 2;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[53], 0x2a);
    pad.battery = 0; pad.charging = 0;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[53], 0x00);
    pad.battery = 99; pad.charging = 0;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[53], 0x09);

    /* disconnected: a pad at rest */
    memset(&pad, 0, sizeof(pad));
    pad.packet = 9;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[0], 1);
    CHECK_EQ(r[1], 128);
    CHECK_EQ(r[2], 128);
    CHECK_EQ(r[3], 128);
    CHECK_EQ(r[4], 128);
    CHECK_EQ(r[5], 0);
    CHECK_EQ(r[6], 0);
    CHECK_EQ(r[8], 0x08);
    CHECK_EQ(r[9], 0);
    CHECK_EQ(r[10], 0);
    CHECK_EQ(le16(r + 24), 8192);
    CHECK_EQ(r[33] & 0x80, 0x80);
    CHECK_EQ(r[37] & 0x80, 0x80);
    CHECK_EQ(r[53], 0x2a);
}

static void test_generic_input(void)
{
    struct ios_hidpad_identity id;
    struct ios_hidpad_report_state st;
    struct pgios_hidpad pad;
    uint8_t r[14];
    int i;

    ios_hidpad_identity_select("generic", NULL, &id);
    memset(&st, 0, sizeof(st));
    memset(&pad, 0, sizeof(pad));
    pad.connected = 1;
    pad.lx = -32768;
    pad.ly = -32768;
    pad.rx = 100;
    pad.ry = 32767;
    pad.left_trigger = 255;
    pad.buttons = DPAD_DOWN | DPAD_LEFT | BTN_A | BTN_Y | BTN_BACK | BTN_GUIDE;

    CHECK_EQ(ios_hidpad_build_input(&id, &pad, &st, 0, r), 14);
    CHECK_EQ(r[0], 1);
    CHECK_EQ(le16(r + 1), -32767);      /* clamped into the symmetric range */
    CHECK_EQ(le16(r + 3), 32767);       /* Y flipped: -32768 becomes 32767 */
    CHECK_EQ(le16(r + 5), 100);
    CHECK_EQ(le16(r + 7), -32767);
    CHECK_EQ(le16(r + 9), 32767);       /* Z = left - right */
    CHECK_EQ(r[11], 6);                 /* SW is 5 + 1 */
    CHECK_EQ(ule16(r + 12), 0x0449);    /* A(0) Y(3) Back(6) Guide(10) */

    pad.left_trigger = 0;
    pad.right_trigger = 255;
    pad.buttons = DPAD_UP | DPAD_DOWN;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(le16(r + 9), -32767);
    CHECK_EQ(r[11], 0);                 /* cancelled: centred (null) */
    CHECK_EQ(ule16(r + 12), 0);

    pad.left_trigger = pad.right_trigger = 128;
    pad.buttons = DPAD_UP;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(le16(r + 9), 0);
    CHECK_EQ(r[11], 1);
    pad.buttons = DPAD_UP | DPAD_LEFT;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[11], 8);
    pad.buttons = 0xffff;
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(ule16(r + 12), 0x07ff);

    /* disconnected: at rest */
    memset(&pad, 0, sizeof(pad));
    pad.lx = 1234;                     /* ignored */
    ios_hidpad_build_input(&id, &pad, &st, 0, r);
    CHECK_EQ(r[0], 1);
    for (i = 1; i < 14; i++) CHECK_EQ(r[i], 0);
}

/* ---- A5 ---------------------------------------------------------------------------------- */

static void test_features(void)
{
    struct ios_hidpad_identity id;
    uint8_t f[64];
    char serial[32];
    int i, numerator, sens;

    ios_hidpad_identity_select("dualsense", NULL, &id);

    memset(f, 0xcc, sizeof(f));
    ios_hidpad_build_feature(&id, 0x05, f, 41);
    CHECK_EQ(f[0], 0x05);
    for (i = 1; i <= 6; i++) CHECK_EQ(f[i], 0);     /* zero bias */
    for (i = 0; i < 3; i++)
    {
        CHECK_EQ(le16(f + 7 + 4 * i), 8640);
        CHECK_EQ(le16(f + 9 + 4 * i), -8640);
        CHECK_EQ(le16(f + 23 + 4 * i), 8192);
        CHECK_EQ(le16(f + 25 + 4 * i), -8192);
    }
    CHECK_EQ(le16(f + 19), 540);
    CHECK_EQ(le16(f + 21), 540);
    for (i = 35; i < 41; i++) CHECK_EQ(f[i], 0);
    CHECK_EQ(f[41], 0xcc);                           /* nothing past the report */
    /* a reader's normalisation (as SDL's driver computes it) is exact */
    numerator = (le16(f + 19) + le16(f + 21)) * 1024;
    sens = numerator / (le16(f + 7) - le16(f + 9));
    CHECK_EQ(sens * 8640 / 1024, 540);               /* 8640 raw = 540 deg/s */
    CHECK_EQ(numerator % (le16(f + 7) - le16(f + 9)), 0);
    CHECK_EQ(le16(f + 23) - (le16(f + 23) - le16(f + 25)) / 2, 0);   /* accel bias */
    CHECK_EQ(2 * 8192 / (le16(f + 23) - le16(f + 25)), 1);           /* accel sensitivity */

    ios_hidpad_build_feature(&id, 0x09, f, 20);
    CHECK_EQ(f[0], 0x09);
    snprintf(serial, sizeof(serial), "%.2x-%.2x-%.2x-%.2x-%.2x-%.2x", f[6], f[5], f[4], f[3], f[2], f[1]);
    CHECK_STR(serial, "02-a1-4d-61-64-65");
    for (i = 7; i < 20; i++) CHECK_EQ(f[i], 0);

    ios_hidpad_build_feature(&id, 0x20, f, 64);
    CHECK_EQ(f[0], 0x20);
    CHECK_EQ(f[44] | (f[45] << 8), 0x0224);
    CHECK(!memcmp(f + 1, "Oct 19 2026", 11));
    CHECK(!memcmp(f + 12, "10:00:00", 8));

    memset(f, 0xcc, sizeof(f));
    ios_hidpad_build_feature(&id, 0x08, f, 48);
    CHECK_EQ(f[0], 0x08);
    for (i = 1; i < 48; i++) CHECK_EQ(f[i], 0);

    ios_hidpad_identity_select("generic", NULL, &id);
    memset(f, 0xcc, sizeof(f));
    ios_hidpad_build_feature(&id, 0x05, f, 41);
    CHECK_EQ(f[0], 0x05);
    CHECK_EQ(f[7], 0);
}

/* ---- A6 ---------------------------------------------------------------------------------- */

static void put_crc(uint8_t *bt)
{
    uint8_t hdr = 0xa2;
    uint32_t crc = ios_hidpad_crc32(0, &hdr, 1);
    crc = ios_hidpad_crc32(crc, bt, 74);
    bt[74] = (uint8_t)crc; bt[75] = (uint8_t)(crc >> 8); bt[76] = (uint8_t)(crc >> 16); bt[77] = (uint8_t)(crc >> 24);
}

static void test_output(void)
{
    struct pgios_hidpad_output out, before;
    struct ios_hidpad_output_flags flags;
    uint8_t usb[48], bt[78];
    uint8_t *c = usb + 1;
    int i;

    CHECK_EQ(ios_hidpad_crc32(0, (const uint8_t *)"123456789", 9), 0xcbf43926u);

    memset(&out, 0, sizeof(out));
    out.serial = 41;
    memset(usb, 0, sizeof(usb));
    usb[0] = 0x02;
    c[0] = 0x01 | 0x04;              /* compatible vibration, right trigger */
    c[1] = 0x01 | 0x04 | 0x10 | 0x40;
    c[2] = 0x40;                     /* right = small */
    c[3] = 0x80;                     /* left = large */
    c[8] = 1;
    for (i = 0; i < 11; i++) c[10 + i] = (uint8_t)(i + 1);
    for (i = 0; i < 11; i++) c[21 + i] = 0xee;   /* not flagged */
    c[36] = 0xff;
    c[43] = 0x04;
    c[44] = 10; c[45] = 20; c[46] = 30;

    CHECK_EQ(ios_hidpad_decode_output(usb, sizeof(usb), &out, &flags), IOS_HIDPAD_OUTPUT_FOLDED);
    CHECK_EQ(out.serial, 41);
    CHECK_EQ(out.rumble_valid, 1);
    CHECK_EQ(out.rumble_right, 0x40);
    CHECK_EQ(out.rumble_left, 0x80);
    CHECK_EQ(out.trigger_valid[1], 1);
    for (i = 0; i < 11; i++) CHECK_EQ(out.trigger[1][i], i + 1);
    CHECK_EQ(out.trigger_valid[0], 0);
    CHECK_EQ(out.trigger[0][0], 0);
    CHECK_EQ(out.mute_led_valid, 1);
    CHECK_EQ(out.mute_led, 1);
    CHECK_EQ(out.power_valid, 1);
    CHECK_EQ(out.power_reduction, 0x77);
    CHECK_EQ(out.player_leds_valid, 1);
    CHECK_EQ(out.player_leds, 4);
    CHECK_EQ(out.lightbar_valid, 1);
    CHECK_EQ(out.red, 10);
    CHECK_EQ(out.green, 20);
    CHECK_EQ(out.blue, 30);
    CHECK_EQ(flags.rumble, 1);
    CHECK_EQ(flags.triggers, 1);
    CHECK_EQ(flags.lightbar, 1);
    CHECK_EQ(flags.leds, 1);
    CHECK_EQ(flags.power, 1);

    /* partial flags keep the old fields */
    before = out;
    memset(usb, 0, sizeof(usb));
    usb[0] = 0x02;
    c[1] = 0x04;
    c[2] = 0x11; c[3] = 0x22;        /* not flagged: ignored */
    c[44] = 1; c[45] = 2; c[46] = 3;
    CHECK_EQ(ios_hidpad_decode_output(usb, sizeof(usb), &out, &flags), IOS_HIDPAD_OUTPUT_FOLDED);
    CHECK_EQ(out.rumble_left, 0x80);
    CHECK_EQ(out.rumble_right, 0x40);
    CHECK_EQ(out.trigger[1][10], 11);
    CHECK_EQ(out.player_leds, 4);
    CHECK_EQ(out.red, 1);
    CHECK_EQ(out.green, 2);
    CHECK_EQ(out.blue, 3);
    CHECK_EQ(flags.rumble, 0);
    CHECK_EQ(flags.lightbar, 1);
    CHECK(!ios_hidpad_output_same(&before, &out));

    /* improved rumble (flags2) and haptics select both carry rumble; left trigger block */
    memset(usb, 0, sizeof(usb));
    usb[0] = 0x02;
    c[38] = 0x04;
    c[2] = 3; c[3] = 4;
    c[0] = 0x08;
    for (i = 0; i < 11; i++) c[21 + i] = (uint8_t)(0x20 + i);
    ios_hidpad_decode_output(usb, sizeof(usb), &out, NULL);
    CHECK_EQ(out.rumble_right, 3);
    CHECK_EQ(out.rumble_left, 4);
    CHECK_EQ(out.trigger_valid[0], 1);
    CHECK_EQ(out.trigger[0][0], 0x20);
    CHECK_EQ(out.trigger[0][10], 0x2a);
    CHECK_EQ(out.trigger[1][0], 1);
    memset(usb, 0, sizeof(usb));
    usb[0] = 0x02;
    c[0] = 0x02;
    ios_hidpad_decode_output(usb, sizeof(usb), &out, NULL);
    CHECK_EQ(out.rumble_valid, 1);
    CHECK_EQ(out.rumble_left, 0);

    /* Bluetooth: common block from byte 3, CRC over 0xA2 and 74 bytes */
    memset(bt, 0, sizeof(bt));
    bt[0] = 0x31;
    bt[1] = 0x02;
    bt[3] = 0x01;                    /* compatible vibration */
    bt[3 + 2] = 0x55;
    bt[3 + 3] = 0x66;
    put_crc(bt);
    CHECK_EQ(ios_hidpad_decode_output(bt, sizeof(bt), &out, NULL), IOS_HIDPAD_OUTPUT_FOLDED);
    CHECK_EQ(out.rumble_right, 0x55);
    CHECK_EQ(out.rumble_left, 0x66);
    before = out;
    bt[3 + 2] = 0x01;                /* payload changed, CRC not updated */
    CHECK_EQ(ios_hidpad_decode_output(bt, sizeof(bt), &out, NULL), IOS_HIDPAD_OUTPUT_BAD_CRC);
    CHECK(!memcmp(&before, &out, sizeof(out)));
    put_crc(bt);
    bt[77] ^= 0x80;                  /* CRC itself damaged */
    CHECK_EQ(ios_hidpad_decode_output(bt, sizeof(bt), &out, NULL), IOS_HIDPAD_OUTPUT_BAD_CRC);
    CHECK_EQ(ios_hidpad_decode_output(bt, 77, &out, NULL), IOS_HIDPAD_OUTPUT_IGNORED);

    /* not an effects report: accepted and ignored */
    before = out;
    memset(usb, 0xff, sizeof(usb));
    usb[0] = 0x05;
    CHECK_EQ(ios_hidpad_decode_output(usb, sizeof(usb), &out, NULL), IOS_HIDPAD_OUTPUT_IGNORED);
    usb[0] = 0x02;
    CHECK_EQ(ios_hidpad_decode_output(usb, 10, &out, NULL), IOS_HIDPAD_OUTPUT_IGNORED);
    CHECK_EQ(ios_hidpad_decode_output(NULL, 0, &out, NULL), IOS_HIDPAD_OUTPUT_IGNORED);
    CHECK(!memcmp(&before, &out, sizeof(out)));

    /* B2 release: motors off, trigger effects released, the rest kept */
    out.rumble_left = 9; out.rumble_right = 9;
    CHECK_EQ(ios_hidpad_output_release(&out), 1);
    CHECK_EQ(out.rumble_valid, 1);
    CHECK_EQ(out.rumble_left, 0);
    CHECK_EQ(out.rumble_right, 0);
    CHECK_EQ(out.trigger_valid[0], 1);
    CHECK_EQ(out.trigger_valid[1], 1);
    for (i = 0; i < 11; i++) CHECK_EQ(out.trigger[0][i] | out.trigger[1][i], 0);
    CHECK_EQ(out.red, 1);
    CHECK_EQ(out.player_leds, 4);
    CHECK_EQ(ios_hidpad_output_release(&out), 0);
    memset(&out, 0, sizeof(out));
    CHECK_EQ(ios_hidpad_output_release(&out), 0);   /* nothing was ever set */
    CHECK_EQ(out.rumble_valid, 0);

    /* comparison ignores the serial */
    before = out;
    before.serial = 99;
    CHECK(ios_hidpad_output_same(&before, &out));
}

/* ---- B3 / B7 ------------------------------------------------------------------------------ */

static void test_pacing(void)
{
    CHECK_EQ(ios_hidpad_read_ready(0, 0, 0, 0, 0), 1);            /* first report: at once */
    CHECK_EQ(ios_hidpad_read_ready(4999, 1, 1000, 5, 5), 0);
    CHECK_EQ(ios_hidpad_read_ready(5000, 1, 1000, 5, 5), 1);      /* 4 ms since the last */
    CHECK_EQ(ios_hidpad_read_ready(1999, 1, 1000, 5, 6), 0);      /* changed, under 1 ms */
    CHECK_EQ(ios_hidpad_read_ready(2000, 1, 1000, 5, 6), 1);      /* changed, 1 ms */
    CHECK_EQ(ios_hidpad_read_ready(2000, 1, 1000, 5, 5), 0);      /* unchanged, 1 ms */
    CHECK_EQ(ios_hidpad_read_ready(500, 1, 1000, 5, 6), 0);       /* clock behind: wait */

    CHECK_EQ(ios_hidpad_effects_log_kind(0), 0);
    CHECK_EQ(ios_hidpad_effects_log_kind(1), 1);
    CHECK_EQ(ios_hidpad_effects_log_kind(8), 1);
    CHECK_EQ(ios_hidpad_effects_log_kind(9), 0);
    CHECK_EQ(ios_hidpad_effects_log_kind(100), 2);
    CHECK_EQ(ios_hidpad_effects_log_kind(101), 0);
    CHECK_EQ(ios_hidpad_effects_log_kind(1000), 2);
    CHECK_EQ(ios_hidpad_effects_log_kind(10000), 2);
    CHECK_EQ(ios_hidpad_effects_log_kind(100000), 0);
}

/* ---- clarification 4 (spec 21 Part B): the gate ------------------------------------------ */

static void test_gate(void)
{
    struct ios_hidpad_identity id;
    struct ios_hidpad_report_state st;
    struct pgios_hidpad pad, rest;
    uint8_t live[IOS_HIDPAD_DUALSENSE_INPUT_LEN], gone[IOS_HIDPAD_DUALSENSE_INPUT_LEN];
    uint8_t at_rest[IOS_HIDPAD_DUALSENSE_INPUT_LEN];
    uint32_t refusals = 0;
    int log_it = 7, i;

    /* connected: opens, nothing counted or logged */
    CHECK_EQ(ios_hidpad_open_allowed(1, &refusals, &log_it), 1);
    CHECK_EQ(refusals, 0);
    CHECK_EQ(log_it, 0);

    /* disconnected: refused, the first 4 refusals logged, later ones not */
    for (i = 1; i <= 6; i++)
    {
        CHECK_EQ(ios_hidpad_open_allowed(0, &refusals, &log_it), 0);
        CHECK_EQ(refusals, i);
        CHECK_EQ(log_it, i <= IOS_HIDPAD_REFUSALS_LOGGED);
    }
    /* connected again: opens; the refusal count (and so the log limit) stays */
    CHECK_EQ(ios_hidpad_open_allowed(1, &refusals, &log_it), 1);
    CHECK_EQ(refusals, 6);
    CHECK_EQ(log_it, 0);
    CHECK_EQ(ios_hidpad_open_allowed(0, &refusals, NULL), 0);
    CHECK_EQ(refusals, 7);
    refusals = UINT32_MAX;
    CHECK_EQ(ios_hidpad_open_allowed(0, &refusals, &log_it), 0);
    CHECK_EQ(refusals, UINT32_MAX);
    CHECK_EQ(log_it, 0);

    /* A handle opened while the pad was there keeps reading after a disconnect: the store's
     * disconnected snapshot (zeros, packet kept) builds the report of a pad at rest. */
    CHECK(ios_hidpad_identity_select("dualsense", NULL, &id));
    memset(&st, 0, sizeof(st));
    memset(&pad, 0, sizeof(pad));
    pad.packet = 41;
    pad.connected = 1;
    pad.buttons = 0x1000 | PGIOS_HIDPAD_R2;
    pad.lx = 30000;
    pad.right_trigger = 255;
    pad.battery = 80;
    ios_hidpad_build_input(&id, &pad, &st, 1000, live);
    memset(&rest, 0, sizeof(rest));
    rest.packet = 42;                       /* what pgios_hidpad_get_state gives when unplugged */
    ios_hidpad_build_input(&id, &rest, &st, 1000, gone);
    CHECK(memcmp(live, gone, sizeof(live)) != 0);
    CHECK_EQ(gone[1], 128);                 /* sticks centred */
    CHECK_EQ(gone[6], 0);                   /* right trigger released */
    CHECK_EQ(gone[8], 0x08);                /* hat centred, no face buttons */
    CHECK_EQ(gone[9], 0);
    /* the same as a fresh device's report of an all-zero snapshot (counter aside) */
    memset(&st, 0, sizeof(st));
    memset(&rest, 0, sizeof(rest));
    ios_hidpad_build_input(&id, &rest, &st, 1000, at_rest);
    gone[7] = at_rest[7] = 0;               /* the per-report counter differs */
    CHECK(memcmp(gone, at_rest, sizeof(gone)) == 0);
}

int main(void)
{
    test_identities();
    test_descriptors();
    test_hat();
    test_dualsense_input();
    test_generic_input();
    test_features();
    test_output();
    test_pacing();
    test_gate();
    return test_report("test_hidpad");
}
