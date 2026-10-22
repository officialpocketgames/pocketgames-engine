// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part A, "Name clashes". ntdll's unix side, win32u's unix side (spec 12) and the
// wineserver (spec 08) are linked into one app binary. These are the external symbols that
// ntdll's unix side defines and one of the others defines too (found by comparing the
// non-static definitions of refs/wine-11.4/dlls/ntdll/unix/*.c, the "makedep unix" files of
// dlls/win32u/ and server/*.c). They are renamed here, on ntdll's side only, so the other
// libraries keep upstream's names. build-ios.sh passes "-include ios_rename.h".
//
// The renames are plain object-like macros applied to every ntdll unix file, so a definition,
// its declaration in unix_private.h and every use change together. Server request names that
// match (get_thread_context, set_thread_context) are unaffected: SERVER_START_REQ pastes its
// argument with ##, which suppresses macro expansion.
//
// The CI link of all the libraries is the check: a new clash shows up as a duplicate symbol and
// gets a line here.

#ifndef MYIOSDECK_IOS_RENAME_H
#define MYIOSDECK_IOS_RENAME_H

#ifdef WINE_IOS

/* also defined by win32u's unix side (dlls/win32u/window.c) */
#define init_startup_info         ntdll_init_startup_info

/* also defined by the wineserver: get/set_thread_context (server/mach.c), native_machine and
 * supported_machines[_count] (server/registry.c), server_start_time (server/request.c),
 * user_shared_data (server/fd.c) */
#define get_thread_context        ntdll_get_thread_context
#define set_thread_context        ntdll_set_thread_context
#define native_machine            ntdll_native_machine
#define supported_machines        ntdll_supported_machines
#define supported_machines_count  ntdll_supported_machines_count
#define server_start_time         ntdll_server_start_time
#define user_shared_data          ntdll_user_shared_data

#endif /* WINE_IOS */

#endif /* MYIOSDECK_IOS_RENAME_H */
