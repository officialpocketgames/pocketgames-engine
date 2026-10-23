/*
 * The Wine project - Xinput Joystick Library
 * Copyright 2008 Andrew Fenn
 * Copyright 2018 Aric Stewart
 * Copyright 2021 Rémi Bernon for CodeWeavers
 *
 * Changed for MYIOSDECK (spec 15 Part D4): the keystroke edge detector of
 * dlls/xinput1_3/main.c (Wine 11.4), moved here as one function over a current state and
 * an edge memory, so that the HID path and the host's controller slots share it and a host
 * test can compile it. Its behaviour is unchanged.
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

#ifndef __WINE_XINPUT_KEYSTROKE_H
#define __WINE_XINPUT_KEYSTROKE_H

/* Needs windef.h, winerror.h and xinput.h. */

static const int JS_STATE_OFF = 0;
static const int JS_STATE_LOW = 1;
static const int JS_STATE_HIGH = 2;

static int joystick_state(const SHORT value)
{
    if (value > 20000) return JS_STATE_HIGH;
    if (value < -20000) return JS_STATE_LOW;
    return JS_STATE_OFF;
}

static WORD js_vk_offs(const int x, const int y)
{
    if (y == JS_STATE_OFF)
    {
      /*if (x == JS_STATE_OFF) shouldn't get here */
        if (x == JS_STATE_LOW) return 3; /* LEFT */
      /*if (x == JS_STATE_HIGH)*/ return 2; /* RIGHT */
    }
    if (y == JS_STATE_HIGH)
    {
        if (x == JS_STATE_OFF) return 0; /* UP */
        if (x == JS_STATE_LOW) return 4; /* UPLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 5; /* UPRIGHT */
    }
  /*if (y == JS_STATE_LOW)*/
    {
        if (x == JS_STATE_OFF) return 1; /* DOWN */
        if (x == JS_STATE_LOW) return 7; /* DOWNLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 6; /* DOWNRIGHT */
    }
}

static DWORD check_joystick_keystroke(const DWORD index, XINPUT_KEYSTROKE *keystroke, const SHORT *cur_x,
                                      const SHORT *cur_y, SHORT *last_x, SHORT *last_y, const WORD base_vk)
{
    int cur_vk = 0, cur_x_st, cur_y_st;
    int last_vk = 0, last_x_st, last_y_st;

    cur_x_st = joystick_state(*cur_x);
    cur_y_st = joystick_state(*cur_y);
    if (cur_x_st || cur_y_st)
        cur_vk = base_vk + js_vk_offs(cur_x_st, cur_y_st);

    last_x_st = joystick_state(*last_x);
    last_y_st = joystick_state(*last_y);
    if (last_x_st || last_y_st)
        last_vk = base_vk + js_vk_offs(last_x_st, last_y_st);

    if (cur_vk != last_vk)
    {
        if (last_vk)
        {
            /* joystick was set, and now different. send a KEYUP event, and set
             * last pos to centered, so the appropriate KEYDOWN event will be
             * sent on the next call. */
            keystroke->VirtualKey = last_vk;
            keystroke->Unicode = 0; /* unused */
            keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
            keystroke->UserIndex = index;
            keystroke->HidCode = 0;

            *last_x = 0;
            *last_y = 0;

            return ERROR_SUCCESS;
        }

        /* joystick was unset, send KEYDOWN. */
        keystroke->VirtualKey = cur_vk;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
        keystroke->UserIndex = index;
        keystroke->HidCode = 0;

        *last_x = *cur_x;
        *last_y = *cur_y;

        return ERROR_SUCCESS;
    }

    *last_x = *cur_x;
    *last_y = *cur_y;

    return ERROR_EMPTY;
}

static BOOL trigger_is_on(const BYTE value)
{
    return value > 30;
}

/* One keystroke event for user `index` from the current state `cur` and the edge memory
 * `last` (updated), or ERROR_EMPTY. */
static DWORD xinput_keystroke_edge(const DWORD index, const XINPUT_GAMEPAD *cur, XINPUT_GAMEPAD *last,
                                   XINPUT_KEYSTROKE *keystroke)
{
    DWORD ret = ERROR_EMPTY;
    int i;

    static const struct
    {
        int mask;
        WORD vk;
    } buttons[] = {
        { XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP },
        { XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT },
        { XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT },
        { XINPUT_GAMEPAD_START, VK_PAD_START },
        { XINPUT_GAMEPAD_BACK, VK_PAD_BACK },
        { XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS },
        { XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER },
        { XINPUT_GAMEPAD_A, VK_PAD_A },
        { XINPUT_GAMEPAD_B, VK_PAD_B },
        { XINPUT_GAMEPAD_X, VK_PAD_X },
        { XINPUT_GAMEPAD_Y, VK_PAD_Y },
        /* note: guide button does not send an event */
    };

    /*** buttons ***/
    for (i = 0; i < ARRAY_SIZE(buttons); ++i)
    {
        if ((cur->wButtons & buttons[i].mask) ^ (last->wButtons & buttons[i].mask))
        {
            keystroke->VirtualKey = buttons[i].vk;
            keystroke->Unicode = 0; /* unused */
            if (cur->wButtons & buttons[i].mask)
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
                last->wButtons |= buttons[i].mask;
            }
            else
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
                last->wButtons &= ~buttons[i].mask;
            }
            keystroke->UserIndex = index;
            keystroke->HidCode = 0;
            return ERROR_SUCCESS;
        }
    }

    /*** triggers ***/
    if (trigger_is_on(cur->bLeftTrigger) ^ trigger_is_on(last->bLeftTrigger))
    {
        keystroke->VirtualKey = VK_PAD_LTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bLeftTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = index;
        keystroke->HidCode = 0;
        last->bLeftTrigger = cur->bLeftTrigger;
        return ERROR_SUCCESS;
    }

    if (trigger_is_on(cur->bRightTrigger) ^ trigger_is_on(last->bRightTrigger))
    {
        keystroke->VirtualKey = VK_PAD_RTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bRightTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = index;
        keystroke->HidCode = 0;
        last->bRightTrigger = cur->bRightTrigger;
        return ERROR_SUCCESS;
    }

    /*** joysticks ***/
    ret = check_joystick_keystroke(index, keystroke, &cur->sThumbLX, &cur->sThumbLY,
            &last->sThumbLX, &last->sThumbLY, VK_PAD_LTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        return ret;

    ret = check_joystick_keystroke(index, keystroke, &cur->sThumbRX, &cur->sThumbRY,
            &last->sThumbRX, &last->sThumbRY, VK_PAD_RTHUMB_UP);
    return ret;
}

#endif /* __WINE_XINPUT_KEYSTROKE_H */
