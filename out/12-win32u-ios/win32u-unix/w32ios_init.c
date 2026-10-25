// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part A2: the unix library's entry points under the names spec 09's loader uses, and
// the iOS additions run from win32u_unix_lib_init (upstream's __wine_unix_lib_init, renamed by
// w32ios_rename.h). Also the log helpers.

#include <stdio.h>
#include <stdlib.h>
#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/unixlib.h"
#include "wine/debug.h"
#include "w32ios.h"

/* Spec 09 Part A binds a DLL's unix library through <name>_unix_call_funcs and
 * <name>_unix_call_wow64_funcs. win32u has no unix calls of its own (its calls are system
 * calls, registered by win32u_unix_lib_init), so the tables hold one entry that answers
 * STATUS_NOT_IMPLEMENTED; they exist so that the lookup succeeds. */
static NTSTATUS unix_call_none( void *args )
{
    return STATUS_NOT_IMPLEMENTED;
}

const unixlib_entry_t win32u_unix_call_funcs[] = { unix_call_none };
const unixlib_entry_t win32u_unix_call_wow64_funcs[] = { unix_call_none };

/* ---------------------------------------------------------------- logging */

static void log_line( const char *fmt, va_list args )
{
    char buffer[1024];
    int len = snprintf( buffer, sizeof(buffer), "[win32u] " );

    len += vsnprintf( buffer + len, sizeof(buffer) - len - 1, fmt, args );
    if (len > (int)sizeof(buffer) - 2) len = sizeof(buffer) - 2;
    if (len && buffer[len - 1] != '\n') buffer[len++] = '\n';
    buffer[len] = 0;
    __wine_dbg_output( buffer );
}

void w32ios_log( const char *fmt, ... )
{
    va_list args;

    va_start( args, fmt );
    log_line( fmt, args );
    va_end( args );
}

void w32ios_log_verbose( const char *fmt, ... )
{
    static int quiet = -1;
    va_list args;

    if (quiet < 0) quiet = w32ios_env_bool( "PG_QUIET", FALSE );
    if (quiet) return;
    va_start( args, fmt );
    log_line( fmt, args );
    va_end( args );
}

BOOL w32ios_env_bool( const char *name, BOOL def )
{
    return w32ios_parse_bool( getenv( name ), def );
}

/* ---------------------------------------------------------------- init */

/* A program with no parent (one the app booted) creates its slot (clarifications 6, 7, 10). */
void w32ios_session_start( void )
{
    w32ios_display_session_start();   /* Part C2 */
    w32ios_driver_session_start();    /* Part E3: no cursor image sent yet */
}

/* Called at the end of win32u_unix_lib_init, every time the loader runs it (once per
 * pseudo-process that loads win32u.dll, QUESTIONS.md Spec 12 question 1). */
void w32ios_unix_lib_init( void )
{
    static LONG logged;

    if (!InterlockedExchange( &logged, 1 )) w32ios_log( "iOS win32u (spec 12) initialised" );

    /* Part E: the iOS user driver, before anything could load another one */
    w32ios_driver_install();

    /* Part B: the caller's per-process slot; creating it publishes the GDI handle table in the
     * caller's PEB before gdi32 can look for it */
    w32ios_proc_current();
}
