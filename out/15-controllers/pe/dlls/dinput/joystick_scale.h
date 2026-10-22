/*  DirectInput HID Joystick device: axis and value scaling
 *
 * Copyright 2021 Rémi Bernon for CodeWeavers
 *
 * Changed for MYIOSDECK (spec 15 Part E4): moved unchanged out of joystick_hid.c (Wine 11.4)
 * so that joystick_ios.c applies DIPROP_RANGE, DIPROP_DEADZONE and DIPROP_SATURATION exactly
 * as the HID joystick does, and a host test can compile it.
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

#ifndef __WINE_DLLS_DINPUT_JOYSTICK_SCALE_H
#define __WINE_DLLS_DINPUT_JOYSTICK_SCALE_H

/* Needs <math.h> (round), MulDiv and struct object_properties (device_private.h). */

static LONG sign_extend( ULONG value, struct object_properties *properties )
{
    UINT sign = 1 << (properties->bit_size - 1);
    if (sign <= 1 || properties->logical_min >= 0) return value;
    return value - ((value & sign) << 1);
}

static LONG scale_value( ULONG value, struct object_properties *properties )
{
    LONG tmp = sign_extend( value, properties ), log_min, log_max, phy_min, phy_max;
    log_min = properties->logical_min;
    log_max = properties->logical_max;
    phy_min = properties->range_min;
    phy_max = properties->range_max;

    if (log_min > tmp || log_max < tmp) return -1; /* invalid / null value */
    return phy_min + MulDiv( tmp - log_min, phy_max - phy_min, log_max - log_min );
}

static LONG scale_axis_value( ULONG value, struct object_properties *properties )
{
    LONG tmp = sign_extend( value, properties ), log_ctr, log_min, log_max, phy_ctr, phy_min, phy_max;
    ULONG bit_max = (1 << properties->bit_size) - 1;

    log_min = properties->logical_min;
    log_max = properties->logical_max;
    phy_min = properties->range_min;
    phy_max = properties->range_max;
    /* xinput HID gamepad have bogus logical value range, let's use the bit range instead */
    if (log_min == 0 && log_max == -1) log_max = bit_max;

    if (phy_min == 0) phy_ctr = phy_max >> 1;
    else phy_ctr = round( (phy_min + phy_max) / 2.0 );
    if (log_min == 0) log_ctr = log_max >> 1;
    else log_ctr = round( (log_min + log_max) / 2.0 );

    tmp -= log_ctr;
    if (tmp <= 0)
    {
        log_max = MulDiv( log_min - log_ctr, properties->deadzone, 10000 );
        log_min = MulDiv( log_min - log_ctr, properties->saturation, 10000 );
        phy_max = phy_ctr;
    }
    else
    {
        log_min = MulDiv( log_max - log_ctr, properties->deadzone, 10000 );
        log_max = MulDiv( log_max - log_ctr, properties->saturation, 10000 );
        phy_min = phy_ctr;
    }

    if (tmp <= log_min) return phy_min;
    if (tmp >= log_max) return phy_max;
    return phy_min + MulDiv( tmp - log_min, phy_max - phy_min, log_max - log_min );
}

#endif /* __WINE_DLLS_DINPUT_JOYSTICK_SCALE_H */
