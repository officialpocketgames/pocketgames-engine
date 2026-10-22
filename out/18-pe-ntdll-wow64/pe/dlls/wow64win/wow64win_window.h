// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part K: the guest window in wow64win.dll, pure parts shared with the host tests.
 *   K2  conversions that keep atoms and resource IDs (values below 0x10000) unchanged;
 *   K5  which argument of the NtUserCall* multiplexers is a pointer, per code;
 *   K7  which messages sent by 32-bit code carry a pointer in lParam / wParam.
 * The message and code names come from the including file (winuser.h and ntuser.h in the PE
 * build, a shim in the tests); the tables use names only.
 */

#ifndef MYIOSDECK_WOW64WIN_WINDOW_H
#define MYIOSDECK_WOW64WIN_WINDOW_H

#include <stdint.h>

/* K2 */

static inline uint64_t w64w_guest_to_host( uint64_t base, uint32_t value )
{
    if (value < 0x10000) return value;  /* NULL, atoms, resource IDs */
    return base + value;
}

static inline uint32_t w64w_host_to_guest( uint64_t base, uint64_t ptr )
{
    if (ptr < 0x10000) return (uint32_t)ptr;
    return (uint32_t)(ptr - base);
}

/* K5 */

#define W64W_ARG1 1u
#define W64W_ARG2 2u

/* NtUserCallOneParam: does the argument convert? */
static inline int w64w_one_param_converts( unsigned int code )
{
    switch (code)
    {
    case NtUserCallOneParam_GetPrimaryMonitorRect:
    case NtUserCallOneParam_GetAsyncKeyboardState:
    case NtUserCallOneParam_D3DKMTOpenAdapterFromGdiDisplayName:
    case NtUserGetDeskPattern:
        return 1;
    default:
        return 0;
    }
}

/* NtUserCallTwoParam: mask of the arguments that convert */
static inline unsigned int w64w_two_param_mask( unsigned int code )
{
    switch (code)
    {
    case NtUserCallTwoParam_GetMonitorInfo:
    case NtUserCallTwoParam_SetIMECompositionRect:
    case NtUserCallTwoParam_SetIconParam:
    case NtUserCallTwoParam_GetGamepadState:
    case NtUserCallTwoParam_GetMenuInfo:
        return W64W_ARG2;
    case NtUserCallTwoParam_MonitorFromRect:
    case NtUserCallTwoParam_GetVirtualScreenRect:
        return W64W_ARG1;
    case NtUserCallTwoParam_AdjustWindowRect:
        return W64W_ARG1 | W64W_ARG2;
    default:
        return 0;
    }
}

/* NtUserCallHwndParam: does param convert? */
static inline int w64w_hwnd_param_converts( unsigned int code )
{
    switch (code)
    {
    case NtUserCallHwndParam_ClientToScreen:
    case NtUserCallHwndParam_ScreenToClient:
    case NtUserCallHwndParam_GetChildRect:
    case NtUserCallHwndParam_GetWindowInfo:
    case NtUserCallHwndParam_GetWindowThread:
    case NtUserCallHwndParam_ExposeWindowSurface:
    case NtUserCallHwndParam_SetRawWindowPos:
    case NtUserCallHwndParam_GetScrollInfo:
    case NtUserCallHwndParam_GetWindowRect:
    case NtUserCallHwndParam_GetClientRect:
    case NtUserCallHwndParam_GetPresentRect:
    case NtUserCallHwndParam_MapWindowPoints:
    case NtUserCallHwndParam_SendHardwareInput:
        return 1;
    default:
        return 0;
    }
}

/* K7 */

static inline int w64w_msg_lparam_is_pointer( unsigned int msg )
{
    switch (msg)
    {
    case WM_NCCREATE:
    case WM_CREATE:
    case WM_GETTEXT:
    case WM_ASKCBFORMATNAME:
    case WM_WININICHANGE:
    case WM_SETTEXT:
    case WM_DEVMODECHANGE:
    case CB_DIR:
    case LB_DIR:
    case LB_ADDFILE:
    case EM_REPLACESEL:
    case WM_GETMINMAXINFO:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_DELETEITEM:
    case WM_COMPAREITEM:
    case WM_WINE_SETWINDOWPOS:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    case WM_COPYDATA:
    case WM_NOTIFY:
    case WM_HELP:
    case WM_STYLECHANGING:
    case WM_STYLECHANGED:
    case WM_NCCALCSIZE:
    case WM_GETDLGCODE:
    case SBM_SETSCROLLINFO:
    case SBM_GETSCROLLINFO:
    case SBM_GETSCROLLBARINFO:
    case EM_GETSEL:
    case SBM_GETRANGE:
    case CB_GETEDITSEL:
    case EM_GETRECT:
    case LB_GETITEMRECT:
    case CB_GETDROPPEDCONTROLRECT:
    case EM_SETRECT:
    case EM_SETRECTNP:
    case EM_GETLINE:
    case EM_SETTABSTOPS:
    case LB_SETTABSTOPS:
    case CB_ADDSTRING:
    case CB_INSERTSTRING:
    case CB_FINDSTRING:
    case CB_FINDSTRINGEXACT:
    case CB_SELECTSTRING:
    case CB_GETLBTEXT:
    case CB_GETCOMBOBOXINFO:
    case LB_ADDSTRING:
    case LB_INSERTSTRING:
    case LB_FINDSTRING:
    case LB_FINDSTRINGEXACT:
    case LB_SELECTSTRING:
    case LB_GETTEXT:
    case LB_GETSELITEMS:
    case WM_NEXTMENU:
    case WM_SIZING:
    case WM_MOVING:
    case WM_MDICREATE:
    case WM_MDIGETACTIVE:
    case WM_DEVICECHANGE:
    case WM_WINE_KEYBOARD_LL_HOOK:
    case WM_WINE_MOUSE_LL_HOOK:
    case WM_PAINTCLIPBOARD:
    case WM_DROPOBJECT:
    case WM_QUERYDROPOBJECT:
    case WM_DRAGLOOP:
    case WM_DRAGSELECT:
    case WM_DRAGMOVE:
        return 1;
    default:
        return 0;
    }
}

static inline int w64w_msg_wparam_is_pointer( unsigned int msg )
{
    switch (msg)
    {
    case EM_GETSEL:
    case SBM_GETRANGE:
    case CB_GETEDITSEL:
        return 1;
    default:
        return 0;
    }
}

#endif /* MYIOSDECK_WOW64WIN_WINDOW_H */
