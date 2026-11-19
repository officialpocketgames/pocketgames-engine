/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Spec 33 B4: the ios_wow_native build's window and monitor queries, over Wine's unix-side
 * win32u. Compiled against Wine 11.4's own headers (include/ntuser.h), whose static inline
 * NtUser* wrappers give the NtUserCall* codes and argument layouts; nothing here sends a message,
 * runs a hook or calls back into guest code. The user32 functions these stand for build their
 * calls the same way (dlls/user32/win.c and sysparams.c); this file passes a DPI of 0, which
 * win32u treats as "no scaling", since the app's displays are not DPI scaled.
 *
 * Only for game threads inside a gate call: win32u needs a Wine thread. The frontend's wrappers
 * (util_ios_wow_native.h) assert that in debug builds.
 */

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"

#include "util_ios_wow_native_user.h"

static void to_native_rect( const RECT *in, struct dxmt_native_rect *out )
{
    out->left = in->left;
    out->top = in->top;
    out->right = in->right;
    out->bottom = in->bottom;
}

int dxmt_native_user_get_client_rect( void *hwnd, struct dxmt_native_rect *rect )
{
    RECT rc = {0};
    if (!NtUserGetClientRect( (HWND)hwnd, &rc, 0 )) return 0;
    if (rect) to_native_rect( &rc, rect );
    return 1;
}

int dxmt_native_user_client_to_screen( void *hwnd, int32_t *x, int32_t *y )
{
    POINT pt;
    pt.x = *x;
    pt.y = *y;
    if (!NtUserClientToScreen( (HWND)hwnd, &pt )) return 0;
    *x = pt.x;
    *y = pt.y;
    return 1;
}

int dxmt_native_user_is_window( void *hwnd )
{
    return NtUserIsWindow( (HWND)hwnd ) != 0;
}

int dxmt_native_user_is_window_visible( void *hwnd )
{
    return NtUserIsWindowVisible( (HWND)hwnd ) != 0;
}

int dxmt_native_user_get_system_metrics( int index )
{
    return NtUserGetSystemMetrics( index );
}

void *dxmt_native_user_monitor_from_window( void *hwnd, uint32_t flags )
{
    return NtUserMonitorFromWindow( (HWND)hwnd, flags );
}

void *dxmt_native_user_monitor_from_point( int32_t x, int32_t y, uint32_t flags )
{
    /* user32's MonitorFromPoint: the one-pixel rect at the point */
    RECT rect;
    rect.left = x;
    rect.top = y;
    rect.right = x + 1;
    rect.bottom = y + 1;
    return NtUserMonitorFromRect( &rect, flags );
}

int dxmt_native_user_get_monitor_info( void *monitor, struct dxmt_native_rect *monitor_rect,
                                       struct dxmt_native_rect *work_rect, uint32_t *flags )
{
    MONITORINFO info;
    info.cbSize = sizeof(info);
    if (!NtUserGetMonitorInfo( (HMONITOR)monitor, &info )) return 0;
    if (monitor_rect) to_native_rect( &info.rcMonitor, monitor_rect );
    if (work_rect) to_native_rect( &info.rcWork, work_rect );
    if (flags) *flags = info.dwFlags;
    return 1;
}

int dxmt_native_user_enum_display_settings( uint32_t mode, struct dxmt_native_display_mode *out )
{
    /* user32's EnumDisplaySettingsExW(NULL, ...): an empty device name is the primary display */
    UNICODE_STRING device = {0};
    DEVMODEW dm;
    memset( &dm, 0, sizeof(dm) );
    dm.dmSize = sizeof(dm);
    if (!NtUserEnumDisplaySettings( &device, mode, &dm, 0 )) return 0;
    out->width = dm.dmPelsWidth;
    out->height = dm.dmPelsHeight;
    out->bits_per_pixel = dm.dmBitsPerPel;
    out->frequency = dm.dmDisplayFrequency;
    return 1;
}
