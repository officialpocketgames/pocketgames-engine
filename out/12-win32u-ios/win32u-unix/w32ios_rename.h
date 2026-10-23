// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included first in every file of the iOS win32u (build-ios.sh: -include), right after
// w32ios_config.h. No Wine types here: it is read before any header.
//
// Spec 12 Part A2: win32u's unix side links into one app binary with ntdll's unix side (spec 09)
// and the wineserver (spec 08). External names that one of those defines too are renamed here,
// on win32u's side. The list comes from comparing the external definitions of upstream
// dlls/win32u, dlls/ntdll/unix and server (Wine 11.4); CI's link of all of them is the check.
// Request names of the server protocol (SERVER_START_REQ( free_user_handle ) and friends) are
// pasted with ## and are not affected by these macros.

#ifndef W32IOS_RENAME_H
#define W32IOS_RENAME_H

/* the unix library's entry point: spec 09's loader has no __wine_unix_lib_init (QUESTIONS.md) */
#define __wine_unix_lib_init        win32u_unix_lib_init

/* also defined by ntdll's unix side */
#define init_startup_info           win32u_init_startup_info

/* also defined by the wineserver */
#define alloc_user_handle           win32u_alloc_user_handle
#define destroy_thread_windows      win32u_destroy_thread_windows
#define free_user_handle            win32u_free_user_handle
#define get_virtual_screen_rect     win32u_get_virtual_screen_rect
#define get_window_thread           win32u_get_window_thread
#define is_desktop_class            win32u_is_desktop_class
#define is_message_class            win32u_is_message_class
#define is_window_visible           win32u_is_window_visible
#define mirror_region               win32u_mirror_region
#define send_notify_message         win32u_send_notify_message
#define shared_session              win32u_shared_session

/* the iOS additions called from the unix library's init (w32ios_init.c) */
extern void w32ios_unix_lib_init( void );

/* Spec 12 Part B1: upstream keeps one zero_bits, set at init for a WoW64 process. Every use now
 * asks for the calling thread's ceiling (w32ios_proc.c). Spec 17 F1: the macro that does this
 * is defined in win32u_private.h (patches/S17-F-gdi-shared.patch), after the server protocol
 * headers, whose request structs have members named zero_bits; it is not defined here. */

#endif /* W32IOS_RENAME_H */
