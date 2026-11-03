// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part E4 (and A4): opengl32's two unix call tables on iOS, under the names spec 09's
// loader looks up for opengl32.dll. Every entry of both points to a wrapper generated from the
// enum make_opengl wrote (gen-entries.sh -> opengl_ios_entries.h), so B1's answers, E1's counts
// and the autorelease pool cover every entry by construction. The wrappers call the upstream
// entries of the generated unix_thunks.c.

#include <stdarg.h>
#include <stddef.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "unixlib.h"
#include "opengl_ios.h"

#define OPENGL_IOS_ENTRY( name ) \
    static NTSTATUS wrap64_##name( void *args ) { return opengl_ios_call( name, 0, args ); } \
    static NTSTATUS wrapwow_##name( void *args ) { return opengl_ios_call( name, 1, args ); }
#include "opengl_ios_entries.h"
#undef OPENGL_IOS_ENTRY

/* spec 09 clarification 6: the loader finds them with dlsym, so they stay exported */
#define OPENGL_IOS_EXPORT __attribute__((visibility("default"), used))

#define OPENGL_IOS_ENTRY( name ) [name] = wrap64_##name,
OPENGL_IOS_EXPORT const unixlib_entry_t opengl32_unix_call_funcs[] =
{
#include "opengl_ios_entries.h"
};
#undef OPENGL_IOS_ENTRY

#define OPENGL_IOS_ENTRY( name ) [name] = wrapwow_##name,
OPENGL_IOS_EXPORT const unixlib_entry_t opengl32_unix_call_wow64_funcs[] =
{
#include "opengl_ios_entries.h"
};
#undef OPENGL_IOS_ENTRY

/* E2: the entries' names, without the enum's "unix_" */
#define OPENGL_IOS_ENTRY( name ) [name] = &#name[5],
const char *const opengl_ios_entry_names[] =
{
#include "opengl_ios_entries.h"
};
#undef OPENGL_IOS_ENTRY

const unsigned int opengl_ios_entry_count = funcs_count;
const unsigned int opengl_ios_swap_index = unix_wglSwapBuffers;

/* every enum entry has its line (no NULL holes), and the tables have the enum's size */
C_ASSERT( OPENGL_IOS_ENTRY_COUNT == funcs_count );
C_ASSERT( ARRAYSIZE(opengl32_unix_call_funcs) == funcs_count );
C_ASSERT( ARRAYSIZE(opengl32_unix_call_wow64_funcs) == funcs_count );
C_ASSERT( ARRAYSIZE(opengl_ios_entry_names) == funcs_count );
/* opengl_ios_pure.h's setup entries */
C_ASSERT( unix_process_attach == OPENGL_IOS_PROCESS_ATTACH );
C_ASSERT( unix_thread_attach == OPENGL_IOS_THREAD_ATTACH );
C_ASSERT( unix_process_detach == OPENGL_IOS_PROCESS_DETACH );
