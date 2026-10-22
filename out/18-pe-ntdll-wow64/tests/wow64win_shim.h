// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Names that wow64win_window.h takes from winuser.h / ntuser.h in the PE build. Public message
 * numbers are Microsoft's; the Wine-private messages and NtUserCall* codes only need distinct
 * values here (the tables use names; a duplicate value would fail to compile as a switch).
 */

#ifndef MYIOSDECK_WOW64WIN_SHIM_H
#define MYIOSDECK_WOW64WIN_SHIM_H

#define WM_CREATE                0x0001
#define WM_SETTEXT               0x000c
#define WM_GETTEXT               0x000d
#define WM_PAINT                 0x000f
#define WM_WININICHANGE          0x001a
#define WM_DEVMODECHANGE         0x001b
#define WM_GETMINMAXINFO         0x0024
#define WM_DRAWITEM              0x002b
#define WM_MEASUREITEM           0x002c
#define WM_DELETEITEM            0x002d
#define WM_SETFONT               0x0030
#define WM_COMPAREITEM           0x0039
#define WM_WINDOWPOSCHANGING     0x0046
#define WM_WINDOWPOSCHANGED      0x0047
#define WM_COPYDATA              0x004a
#define WM_NOTIFY                0x004e
#define WM_HELP                  0x0053
#define WM_STYLECHANGING         0x007c
#define WM_STYLECHANGED          0x007d
#define WM_NCCREATE              0x0081
#define WM_NCCALCSIZE            0x0083
#define WM_GETDLGCODE            0x0087
#define EM_GETSEL                0x00b0
#define EM_GETRECT               0x00b2
#define EM_SETRECT               0x00b3
#define EM_SETRECTNP             0x00b4
#define EM_REPLACESEL            0x00c2
#define EM_GETLINE               0x00c4
#define EM_SETTABSTOPS           0x00cb
#define SBM_GETRANGE             0x00e3
#define SBM_SETSCROLLINFO        0x00e9
#define SBM_GETSCROLLINFO        0x00ea
#define SBM_GETSCROLLBARINFO     0x00eb
#define WM_KEYDOWN               0x0100
#define WM_COMMAND               0x0111
#define WM_TIMER                 0x0113
#define CB_GETEDITSEL            0x0140
#define CB_ADDSTRING             0x0143
#define CB_DIR                   0x0145
#define CB_GETLBTEXT             0x0148
#define CB_INSERTSTRING          0x014a
#define CB_FINDSTRING            0x014c
#define CB_SELECTSTRING          0x014d
#define CB_GETDROPPEDCONTROLRECT 0x0152
#define CB_FINDSTRINGEXACT       0x0158
#define CB_GETCOMBOBOXINFO       0x0164
#define LB_ADDSTRING             0x0180
#define LB_INSERTSTRING          0x0181
#define LB_GETTEXT               0x0189
#define LB_GETCOUNT              0x018b
#define LB_SELECTSTRING          0x018c
#define LB_DIR                   0x018d
#define LB_FINDSTRING            0x018f
#define LB_GETSELITEMS           0x0191
#define LB_SETTABSTOPS           0x0192
#define LB_ADDFILE               0x0196
#define LB_GETITEMRECT           0x0198
#define LB_FINDSTRINGEXACT       0x01a2
#define WM_MOUSEMOVE             0x0200
#define WM_NEXTMENU              0x0213
#define WM_SIZING                0x0214
#define WM_MOVING                0x0216
#define WM_DEVICECHANGE          0x0219
#define WM_MDICREATE             0x0220
#define WM_MDIGETACTIVE          0x0229
#define WM_DROPOBJECT            0x022a
#define WM_QUERYDROPOBJECT       0x022b
#define WM_DRAGLOOP              0x022d
#define WM_DRAGSELECT            0x022e
#define WM_DRAGMOVE              0x022f
#define WM_PAINTCLIPBOARD        0x0309
#define WM_ASKCBFORMATNAME       0x030c
#define WM_USER                  0x0400

enum shim_wine_messages
{
    WM_WINE_SETWINDOWPOS = 0x7f000001,
    WM_WINE_KEYBOARD_LL_HOOK,
    WM_WINE_MOUSE_LL_HOOK,
    WM_WINE_OTHER_SHIM,
};

enum shim_one_param
{
    NtUserCallOneParam_EnableThunkLock,
    NtUserCallOneParam_GetPrimaryMonitorRect,
    NtUserCallOneParam_D3DKMTOpenAdapterFromGdiDisplayName,
    NtUserCallOneParam_GetAsyncKeyboardState,
    NtUserGetDeskPattern,
    NtUserCallOneParam_OtherShim,
};

enum shim_two_param
{
    NtUserCallTwoParam_GetDialogProc,
    NtUserCallTwoParam_GetMenuInfo,
    NtUserCallTwoParam_GetMonitorInfo,
    NtUserCallTwoParam_MonitorFromRect,
    NtUserCallTwoParam_SetIconParam,
    NtUserCallTwoParam_SetIMECompositionRect,
    NtUserCallTwoParam_AdjustWindowRect,
    NtUserCallTwoParam_GetVirtualScreenRect,
    NtUserAllocWinProc,
    NtUserCallTwoParam_GetGamepadState,
};

enum shim_hwnd_param
{
    NtUserCallHwndParam_ClientToScreen,
    NtUserCallHwndParam_GetChildRect,
    NtUserCallHwndParam_GetScrollInfo,
    NtUserCallHwndParam_GetWindowInfo,
    NtUserCallHwndParam_GetWindowRect,
    NtUserCallHwndParam_GetClientRect,
    NtUserCallHwndParam_GetPresentRect,
    NtUserCallHwndParam_GetWindowThread,
    NtUserCallHwndParam_MapWindowPoints,
    NtUserCallHwndParam_ScreenToClient,
    NtUserCallHwndParam_SetDialogInfo,
    NtUserCallHwndParam_SetMDIClientInfo,
    NtUserCallHwndParam_SendHardwareInput,
    NtUserCallHwndParam_ExposeWindowSurface,
    NtUserCallHwndParam_SetRawWindowPos,
    NtUserCallHwndParam_OtherShim,
};

#endif
