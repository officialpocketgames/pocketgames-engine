// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The app's side of interfaces/wine_unix_contract.h, for files of the iOS win32u that call it.
// Include after Wine's headers (the contract uses Wine's HWND, BOOL, RECT, ... here).
//
// The hooks the contract marks weak are declared weak again: when the app doesn't provide one,
// its address is NULL and the caller checks before calling.

#ifndef W32IOS_APP_H
#define W32IOS_APP_H

#include "wine_unix_contract.h"

#define W32IOS_WEAK __attribute__((weak_import))

/* Section B: user driver hooks */
BOOL winios_pCreateWindow( HWND hwnd ) W32IOS_WEAK;
void winios_pDestroyWindow( HWND hwnd ) W32IOS_WEAK;
UINT winios_pShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT swp ) W32IOS_WEAK;
void winios_pWindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                               const struct window_rects *new_rects, struct window_surface *surface ) W32IOS_WEAK;
BOOL winios_pProcessEvents( DWORD mask ) W32IOS_WEAK;
void winios_pSetCursor( HWND hwnd, HCURSOR cursor ) W32IOS_WEAK;
void winios_pDestroyCursorIcon( HCURSOR cursor ) W32IOS_WEAK;

/* Section B: window images and the cursor */
int  winios_surface_present( HWND hwnd, int dirty_x, int dirty_y, int dirty_w, int dirty_h,
                             int surf_w, int surf_h, int stride, const void *bits ) W32IOS_WEAK;
void winios_window_frame( HWND hwnd, int x, int y, int w, int h, int visible,
                          int cx, int cy, int cw, int ch ) W32IOS_WEAK;
void winios_cursor_set( unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra ) W32IOS_WEAK;
void winios_cursor_show( int show ) W32IOS_WEAK;
int  winios_direct_cursor_wanted( void ) W32IOS_WEAK;
void winios_direct_cursor_set( unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra ) W32IOS_WEAK;
void winios_direct_cursor_show( int show ) W32IOS_WEAK;
void winios_direct_cursor_pos( int x, int y ) W32IOS_WEAK;

/* Section B: display */
void winios_display_mode_changed( int w, int h ) W32IOS_WEAK;

#endif /* W32IOS_APP_H */
