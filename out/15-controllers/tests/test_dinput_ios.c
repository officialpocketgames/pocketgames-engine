// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part F: the DirectInput joystick of Part E: the object table, the values a game
// reads at rest under 0..65535 and -1000..1000, full deflection, dead zone and saturation
// (through dinput's own scaling, pe/dlls/dinput/joystick_scale.h), the POV and the trigger
// buttons. No Wine headers: the few types the scaling code needs are declared here.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int32_t LONG;
typedef uint32_t ULONG;
typedef unsigned int UINT;
typedef uint32_t DWORD;
typedef uintptr_t UINT_PTR;

/* The fields of dinput's struct object_properties (device_private.h) the scaling reads. */
struct object_properties
{
    LONG bit_size;
    LONG physical_min;
    LONG physical_max;
    LONG logical_min;
    LONG logical_max;
    LONG range_min;
    LONG range_max;
    LONG deadzone;
    LONG saturation;
    UINT_PTR app_data;
    DWORD calibration_mode;
    DWORD granularity;
    DWORD scan_code;
};

/* MulDiv as documented: a * b / c with a 64-bit intermediate, rounded to the nearest integer
 * (halves away from zero); -1 when c is 0 or the result doesn't fit. */
static int MulDiv(int a, int b, int c)
{
    long long n, r;

    if (!c) return -1;
    n = (long long)a * b;
    if (c < 0) { n = -n; c = -c; }
    r = n >= 0 ? (n + c / 2) / c : (n - c / 2) / c;
    if (r > 2147483647LL || r < -2147483647LL) return -1;
    return (int)r;
}

#include "joystick_scale.h"
#include "joystick_ios_map.h"
#include "test_common.h"

static struct object_properties props[IOS_JOY_OBJECTS];

/* What joystick_ios.c's init_object_properties sets, then a game's DIPROP_RANGE. */
static void init_props(LONG range_min, LONG range_max)
{
    int i;

    memset(props, 0, sizeof(props));
    for (i = 0; i < IOS_JOY_OBJECTS; i++)
    {
        struct object_properties *p = &props[i];
        if (ios_joy_objects[i].kind == IOS_JOY_AXIS)
        {
            p->bit_size = IOS_JOY_AXIS_BITS;
            p->logical_min = p->physical_min = IOS_JOY_AXIS_LOGICAL_MIN;
            p->logical_max = p->physical_max = IOS_JOY_AXIS_LOGICAL_MAX;
            p->range_min = range_min;
            p->range_max = range_max;
        }
        else if (ios_joy_objects[i].kind == IOS_JOY_POV)
        {
            p->bit_size = IOS_JOY_POV_BITS;
            p->logical_max = IOS_JOY_POV_LOGICAL_MAX;
            p->physical_max = p->range_max = IOS_JOY_POV_RANGE_MAX;
        }
        p->saturation = 10000;
        p->granularity = 1;
    }
}

static LONG read_value(const struct ios_joy_sample *s, int i)
{
    return ios_joy_scaled_value(&props[i], &ios_joy_objects[i], s->value[i]);
}

static void sample(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                   struct ios_joy_sample *s)
{
    ios_joy_from_gamepad(buttons, lt, rt, lx, ly, rx, ry, s);
}

static void test_table(void)
{
    static const uint16_t axis_usages[6] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35 };
    int i;

    CHECK_EQ(IOS_JOY_OBJECTS, 19);
    for (i = 0; i < 6; i++)
    {
        CHECK_EQ(ios_joy_objects[i].kind, IOS_JOY_AXIS);
        CHECK_EQ(ios_joy_objects[i].instance, i);
        CHECK_EQ(ios_joy_objects[i].offset, 4 * i);
        CHECK_EQ(ios_joy_objects[i].usage_page, 1);
        CHECK_EQ(ios_joy_objects[i].usage, axis_usages[i]);
    }
    CHECK_STR(ios_joy_objects[0].name, "X Axis");
    CHECK_STR(ios_joy_objects[5].name, "Z Rotation");
    CHECK_EQ(ios_joy_objects[6].kind, IOS_JOY_POV);
    CHECK_EQ(ios_joy_objects[6].instance, 0);
    CHECK_EQ(ios_joy_objects[6].offset, 24);
    CHECK_EQ(ios_joy_objects[6].usage, 0x39);
    for (i = 0; i < 12; i++)
    {
        CHECK_EQ(ios_joy_objects[7 + i].kind, IOS_JOY_BUTTON);
        CHECK_EQ(ios_joy_objects[7 + i].instance, i);
        CHECK_EQ(ios_joy_objects[7 + i].offset, 28 + i);
        CHECK_EQ(ios_joy_objects[7 + i].usage_page, 9);
        CHECK_EQ(ios_joy_objects[7 + i].usage, i + 1);
    }
    CHECK_STR(ios_joy_objects[18].name, "Button 11");
}

static void test_rest(void)
{
    struct ios_joy_sample s;
    int i;

    sample(0, 0, 0, 0, 0, 0, 0, &s);
    for (i = 0; i < 6; i++) CHECK_EQ(s.value[i], 0);
    CHECK_EQ(s.value[6], IOS_JOY_POV_CENTRED);
    for (i = 7; i < 19; i++) CHECK_EQ(s.value[i], 0);

    init_props(0, 65535);
    for (i = 0; i < 6; i++) CHECK_EQ(read_value(&s, i), 32767);
    CHECK_EQ(read_value(&s, 6), -1);

    init_props(-1000, 1000);
    for (i = 0; i < 6; i++) CHECK_EQ(read_value(&s, i), 0);
    CHECK_EQ(read_value(&s, 6), -1);

    /* a dead zone and a saturation don't move the centre */
    init_props(0, 65535);
    for (i = 0; i < 6; i++) { props[i].deadzone = 1500; props[i].saturation = 9000; }
    for (i = 0; i < 6; i++) CHECK_EQ(read_value(&s, i), 32767);
}

static void test_axes(void)
{
    struct ios_joy_sample s;

    init_props(0, 65535);
    sample(0, 0, 0, 32767, 32767, -32768, -32768, &s);
    CHECK_EQ(s.value[0], 32767);
    CHECK_EQ(s.value[1], -32767);           /* up is negative */
    CHECK_EQ(s.value[3], -32767);           /* -32768 clamped */
    CHECK_EQ(s.value[4], 32767);            /* -32768 (down) flips to 32767 */
    CHECK_EQ(s.value[5], 32767);            /* Rz is the right stick's vertical again */
    CHECK_EQ(read_value(&s, 0), 65535);
    CHECK_EQ(read_value(&s, 1), 0);
    CHECK_EQ(read_value(&s, 3), 0);
    CHECK_EQ(read_value(&s, 4), 65535);
    CHECK_EQ(read_value(&s, 5), 65535);

    init_props(-1000, 1000);
    CHECK_EQ(read_value(&s, 0), 1000);
    CHECK_EQ(read_value(&s, 1), -1000);
    CHECK_EQ(read_value(&s, 3), -1000);
    CHECK_EQ(read_value(&s, 4), 1000);

    /* Z: left trigger minus right trigger, x 128 */
    init_props(0, 65535);
    sample(0, 255, 0, 0, 0, 0, 0, &s);
    CHECK_EQ(s.value[2], 32640);
    CHECK_EQ(read_value(&s, 2), 65408);
    sample(0, 0, 255, 0, 0, 0, 0, &s);
    CHECK_EQ(s.value[2], -32640);
    CHECK(read_value(&s, 2) < 200);
    sample(0, 128, 128, 0, 0, 0, 0, &s);
    CHECK_EQ(read_value(&s, 2), 32767);

    /* dead zone 20 %: a small push stays centred, a full one reaches the end */
    for (int i = 0; i < 6; i++) props[i].deadzone = 2000;
    sample(0, 0, 0, 6000, 0, 0, 0, &s);
    CHECK_EQ(read_value(&s, 0), 32767);
    sample(0, 0, 0, 32767, 0, 0, 0, &s);
    CHECK_EQ(read_value(&s, 0), 65535);
    /* saturation 50 %: half way is already the end */
    for (int i = 0; i < 6; i++) { props[i].deadzone = 0; props[i].saturation = 5000; }
    sample(0, 0, 0, 16384, 0, 0, 0, &s);
    CHECK_EQ(read_value(&s, 0), 65535);
}

static void test_pov_and_buttons(void)
{
    static const struct { uint16_t dpad; LONG value; } povs[] =
    {
        { 0x0001, 0 }, { 0x0009, 4500 }, { 0x0008, 9000 }, { 0x000a, 13500 }, { 0x0002, 18000 },
        { 0x0006, 22500 }, { 0x0004, 27000 }, { 0x0005, 31500 }, { 0x0000, -1 },
        { 0x0003, -1 }, { 0x000c, -1 }, { 0x000f, -1 }, { 0x0007, 27000 },
    };
    struct ios_joy_sample s;
    unsigned int i;

    init_props(0, 65535);
    for (i = 0; i < sizeof(povs) / sizeof(povs[0]); i++)
    {
        sample(povs[i].dpad, 0, 0, 0, 0, 0, 0, &s);
        CHECK_EQ(read_value(&s, 6), povs[i].value);
    }

    /* A B X Y LB RB Back Start L3 R3 */
    sample(0x1000 | 0x8000 | 0x0020 | 0x0080, 0, 0, 0, 0, 0, 0, &s);
    CHECK_EQ(s.value[7], 0x80);    /* A */
    CHECK_EQ(s.value[8], 0);
    CHECK_EQ(s.value[9], 0);
    CHECK_EQ(s.value[10], 0x80);   /* Y */
    CHECK_EQ(s.value[13], 0x80);   /* Back */
    CHECK_EQ(s.value[14], 0);      /* Start */
    CHECK_EQ(s.value[16], 0x80);   /* R3 */
    CHECK_EQ(read_value(&s, 7), 0x80);
    sample(0x2000 | 0x4000 | 0x0100 | 0x0200 | 0x0010 | 0x0040, 0, 0, 0, 0, 0, 0, &s);
    CHECK_EQ(s.value[8] & s.value[9] & s.value[11] & s.value[12] & s.value[14] & s.value[15], 0x80);
    CHECK_EQ(s.value[7] | s.value[10] | s.value[13] | s.value[16], 0);

    /* trigger buttons: past XInput's threshold (30) */
    sample(0, 30, 31, 0, 0, 0, 0, &s);
    CHECK_EQ(s.value[17], 0);
    CHECK_EQ(s.value[18], 0x80);
    sample(0, 255, 0, 0, 0, 0, 0, &s);
    CHECK_EQ(s.value[17], 0x80);
    CHECK_EQ(s.value[18], 0);
    /* the guide button maps to nothing */
    sample(0x0400, 0, 0, 0, 0, 0, 0, &s);
    for (i = 7; i < 19; i++) CHECK_EQ(s.value[i], 0);
}

int main(void)
{
    test_table();
    test_rest();
    test_axes();
    test_pov_and_buttons();
    return test_report("test_dinput_ios");
}
