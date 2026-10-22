// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part E3/E4: the iOS DirectInput joystick's objects and how a host sample becomes
// their logical values. Plain C (no Windows headers) so the host tests compile it as it is;
// joystick_ios.c turns the table into DIDEVICEOBJECTINSTANCEW entries and lets dinput's own
// scaling (joystick_scale.h) map the logical values to the game's ranges.

#ifndef MYIOSDECK_JOYSTICK_IOS_MAP_H
#define MYIOSDECK_JOYSTICK_IOS_MAP_H

#include <stdint.h>

enum ios_joy_kind { IOS_JOY_AXIS, IOS_JOY_POV, IOS_JOY_BUTTON };

struct ios_joy_object
{
    enum ios_joy_kind kind;
    unsigned int instance;      /* DIDFT_MAKEINSTANCE number */
    unsigned int offset;        /* in the device state: LONGs from 0, then button bytes */
    uint16_t usage_page;
    uint16_t usage;
    const char *name;
};

#define IOS_JOY_AXES     6
#define IOS_JOY_BUTTONS  12
#define IOS_JOY_OBJECTS  (IOS_JOY_AXES + 1 + IOS_JOY_BUTTONS)

/* E3: in this order; the order is the device state's layout. */
static const struct ios_joy_object ios_joy_objects[IOS_JOY_OBJECTS] =
{
    { IOS_JOY_AXIS,   0,  0, 0x01, 0x30, "X Axis" },        /* left stick horizontal */
    { IOS_JOY_AXIS,   1,  4, 0x01, 0x31, "Y Axis" },        /* left stick vertical, down + */
    { IOS_JOY_AXIS,   2,  8, 0x01, 0x32, "Z Axis" },        /* left trigger - right trigger */
    { IOS_JOY_AXIS,   3, 12, 0x01, 0x33, "X Rotation" },    /* right stick horizontal */
    { IOS_JOY_AXIS,   4, 16, 0x01, 0x34, "Y Rotation" },    /* right stick vertical, down + */
    { IOS_JOY_AXIS,   5, 20, 0x01, 0x35, "Z Rotation" },    /* the right stick's vertical again */
    { IOS_JOY_POV,    0, 24, 0x01, 0x39, "Hat Switch" },    /* the d-pad */
    { IOS_JOY_BUTTON, 0, 28, 0x09, 1, "Button 0" },         /* A */
    { IOS_JOY_BUTTON, 1, 29, 0x09, 2, "Button 1" },         /* B */
    { IOS_JOY_BUTTON, 2, 30, 0x09, 3, "Button 2" },         /* X */
    { IOS_JOY_BUTTON, 3, 31, 0x09, 4, "Button 3" },         /* Y */
    { IOS_JOY_BUTTON, 4, 32, 0x09, 5, "Button 4" },         /* LB */
    { IOS_JOY_BUTTON, 5, 33, 0x09, 6, "Button 5" },         /* RB */
    { IOS_JOY_BUTTON, 6, 34, 0x09, 7, "Button 6" },         /* Back */
    { IOS_JOY_BUTTON, 7, 35, 0x09, 8, "Button 7" },         /* Start */
    { IOS_JOY_BUTTON, 8, 36, 0x09, 9, "Button 8" },         /* L3 */
    { IOS_JOY_BUTTON, 9, 37, 0x09, 10, "Button 9" },        /* R3 */
    { IOS_JOY_BUTTON, 10, 38, 0x09, 11, "Button 10" },      /* left trigger past the threshold */
    { IOS_JOY_BUTTON, 11, 39, 0x09, 12, "Button 11" },      /* right trigger past the threshold */
};

/* E4: every axis is centred at rest, so its logical range is signed and symmetric
 * (QUESTIONS 15-1); dinput's default range then puts the rest value at 32767. */
#define IOS_JOY_AXIS_BITS        16
#define IOS_JOY_AXIS_LOGICAL_MIN (-32767)
#define IOS_JOY_AXIS_LOGICAL_MAX 32767
#define IOS_JOY_AXIS_RANGE_MAX   65535
/* POV: logical 0-7 clockwise from north; 8 (centred) is outside the range and reads -1.
 * The range 0..31500 gives steps of 4500. */
#define IOS_JOY_POV_BITS         4
#define IOS_JOY_POV_LOGICAL_MAX  7
#define IOS_JOY_POV_CENTRED      8
#define IOS_JOY_POV_RANGE_MAX    31500

#define IOS_JOY_TRIGGER_THRESHOLD 30    /* XINPUT_GAMEPAD_TRIGGER_THRESHOLD */

/* One host sample (an XINPUT_GAMEPAD's fields) as the objects' logical values: axes signed
 * (-32767..32767), the POV 0-8, buttons 0 or 0x80. */
struct ios_joy_sample
{
    int32_t value[IOS_JOY_OBJECTS];
};

static inline int32_t ios_joy_sym(int16_t v)
{
    return v == -32768 ? -32767 : v;
}

/* DirectInput's down is positive: negate, clamping -32768. */
static inline int32_t ios_joy_down(int16_t v)
{
    return v == -32768 ? 32767 : -(int32_t)v;
}

/* The d-pad bits (XINPUT_GAMEPAD_DPAD_UP 1, DOWN 2, LEFT 4, RIGHT 8) as 0-7 clockwise from
 * north, 8 centred; opposite directions cancel. */
static inline int32_t ios_joy_pov(uint16_t buttons)
{
    static const int8_t table[3][3] = { { 5, 4, 3 }, { 6, 8, 2 }, { 7, 0, 1 } };
    int v = !!(buttons & 0x0001) - !!(buttons & 0x0002);
    int h = !!(buttons & 0x0008) - !!(buttons & 0x0004);
    return table[v + 1][h + 1];
}

static inline void ios_joy_from_gamepad(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                                 int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                                 struct ios_joy_sample *out)
{
    static const uint16_t masks[10] =
    {
        0x1000 /* A */, 0x2000 /* B */, 0x4000 /* X */, 0x8000 /* Y */,
        0x0100 /* LB */, 0x0200 /* RB */, 0x0020 /* Back */, 0x0010 /* Start */,
        0x0040 /* L3 */, 0x0080 /* R3 */,
    };
    int i;

    out->value[0] = ios_joy_sym(lx);
    out->value[1] = ios_joy_down(ly);
    out->value[2] = ((int32_t)left_trigger - (int32_t)right_trigger) * 128;
    out->value[3] = ios_joy_sym(rx);
    out->value[4] = ios_joy_down(ry);
    out->value[5] = ios_joy_down(ry);
    out->value[6] = ios_joy_pov(buttons);
    for (i = 0; i < 10; i++) out->value[7 + i] = (buttons & masks[i]) ? 0x80 : 0;
    out->value[17] = left_trigger > IOS_JOY_TRIGGER_THRESHOLD ? 0x80 : 0;
    out->value[18] = right_trigger > IOS_JOY_TRIGGER_THRESHOLD ? 0x80 : 0;
}

#ifdef __WINE_DLLS_DINPUT_JOYSTICK_SCALE_H
/* The value a game reads for object `object` with logical value `logical`: axes and the POV
 * through dinput's own scaling (axes as 16-bit report values, sign-extended there), buttons
 * as they are. Needs joystick_scale.h (and its struct object_properties) included first. */
static inline LONG ios_joy_scaled_value( struct object_properties *properties,
                                         const struct ios_joy_object *object, int32_t logical )
{
    switch (object->kind)
    {
    case IOS_JOY_AXIS: return scale_axis_value( (uint16_t)logical, properties );
    case IOS_JOY_POV: return scale_value( (ULONG)logical, properties );   /* 8 is out of range: -1 */
    case IOS_JOY_BUTTON: break;
    }
    return logical;
}
#endif

#endif
