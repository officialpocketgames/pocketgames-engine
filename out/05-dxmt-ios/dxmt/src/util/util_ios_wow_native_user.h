/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Spec 33 B4: the Wine unix-side queries the ios_wow_native build of the Direct3D 9 frontend may
 * make on a game thread inside a gate call. None of them sends a message, runs a hook or calls
 * back into guest code. util_ios_wow_native_user.c implements them over Wine 11.4's win32u
 * (include/ntuser.h's NtUserCall* wrappers and NtUserEnumDisplaySettings), compiled against
 * Wine's own headers; this header uses plain C types so the frontend, which is compiled against
 * DXMT's native Windows headers, can call them.
 *
 * Window and monitor handles are user handles: 32-bit values, the same number for the 32-bit
 * program and for native code (spec 33 F1).
 */
#ifndef DXMT_UTIL_IOS_WOW_NATIVE_USER_H
#define DXMT_UTIL_IOS_WOW_NATIVE_USER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dxmt_native_rect
{
    int32_t left, top, right, bottom;
};

struct dxmt_native_display_mode
{
    uint32_t width, height, bits_per_pixel, frequency;
};

/* GetClientRect; 0 on failure. */
int dxmt_native_user_get_client_rect( void *hwnd, struct dxmt_native_rect *rect );
/* ClientToScreen of one point; 0 on failure. */
int dxmt_native_user_client_to_screen( void *hwnd, int32_t *x, int32_t *y );
/* IsWindow, IsWindowVisible. */
int dxmt_native_user_is_window( void *hwnd );
int dxmt_native_user_is_window_visible( void *hwnd );
/* GetSystemMetrics. */
int dxmt_native_user_get_system_metrics( int index );
/* MonitorFromWindow, MonitorFromPoint (MONITOR_DEFAULTTO* flags). */
void *dxmt_native_user_monitor_from_window( void *hwnd, uint32_t flags );
void *dxmt_native_user_monitor_from_point( int32_t x, int32_t y, uint32_t flags );
/* GetMonitorInfo; any output pointer may be NULL. 0 on failure. */
int dxmt_native_user_get_monitor_info( void *monitor, struct dxmt_native_rect *monitor_rect,
                                       struct dxmt_native_rect *work_rect, uint32_t *flags );
/* EnumDisplaySettingsEx(NULL, mode, ..., 0) of the primary display; mode is an index or
   ENUM_CURRENT_SETTINGS / ENUM_REGISTRY_SETTINGS. 0 when there is no such mode. */
int dxmt_native_user_enum_display_settings( uint32_t mode, struct dxmt_native_display_mode *out );

#ifdef __cplusplus
}
#endif

#endif /* DXMT_UTIL_IOS_WOW_NATIVE_USER_H */
