// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part D2: the app's window-to-layer table, `macdrv_functions`, which DXMT's unix half
// already uses. Only its layout is declared here, field for field as upstream DXMT declares
// struct macdrv_functions_t and struct macdrv_win_data
// (refs/dxmt-upstream-a3a6e8f/src/winemetal/unix/winemetal_unix.c, MIT), with the opaque handle
// types as void pointers and BOOL as int (Wine's BOOL), so it needs no Wine or Apple header.

#ifndef W32IOS_MACDRV_H
#define W32IOS_MACDRV_H

struct w32ios_macdrv_win_data
{
    void *hwnd;               /* the window this data belongs to */
    void *cocoa_window;
    void *cocoa_view;
    void *client_cocoa_view;  /* the view a Metal view is made on */
};

struct w32ios_macdrv_functions
{
    void (*macdrv_init_display_devices)( int force );
    struct w32ios_macdrv_win_data *(*get_win_data)( void *hwnd );
    void (*release_win_data)( struct w32ios_macdrv_win_data *data );
    void *(*macdrv_get_cocoa_window)( void *hwnd, int require_on_screen );
    void *(*macdrv_create_metal_device)( void );
    void (*macdrv_release_metal_device)( void *device );
    void *(*macdrv_view_create_metal_view)( void *view, void *device );
    void *(*macdrv_view_get_metal_layer)( void *metal_view );
    void (*macdrv_view_release_metal_view)( void *metal_view );
    void (*on_main_thread)( void *block );
};

#endif /* W32IOS_MACDRV_H */
