// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarification 8: opengl32's unix call tables on iOS, where there is no OpenGL.
//
// Wine's real opengl32 unix side isn't built. Programs that only link or probe opengl32.dll must
// still start, so its DLL init has to find the two tables. unix_process_attach,
// unix_thread_attach and unix_process_detach succeed; every other entry returns
// STATUS_NOT_SUPPORTED (so unix_get_pixel_formats reports no formats and wglCreateContext fails
// as on a machine with no OpenGL driver).

#include "unixlib.h"   /* Wine 11.4 dlls/opengl32/unixlib.h: enum unix_funcs, funcs_count */

#define W32IOS_TABLE_FUNCS   opengl32_unix_call_funcs
#define W32IOS_TABLE_WOW64   opengl32_unix_call_wow64_funcs
#define W32IOS_TABLE_COUNT   funcs_count
#define W32IOS_TABLE_ID      0
#define W32IOS_TABLE_HEAD    "[opengl32] no OpenGL on iOS"
#define W32IOS_TABLE_ENTRIES "opengl32_ios_entries.h"
#include "unix_table_ios.inc"
