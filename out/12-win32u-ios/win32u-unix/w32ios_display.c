// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part C: the size of the one virtual monitor.
//
// The session default is PG_SCREEN_W x PG_SCREEN_H (1024 x 768 when unset or not
// positive), read when a program with no parent creates its slot (w32ios_proc.c, clarifications
// 6, 7 and 10). The current size starts there and otherwise changes only through
// ChangeDisplaySettingsEx (sysparams_ios.inc). Both are single atomic words, so code that holds
// the display lock can read them.

#include <stdlib.h>
#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/debug.h"
#include "w32ios.h"
#include "w32ios_app.h"

static LONG64 default_size;   /* w << 32 | h, 0 before the first session starts */
static LONG64 current_size;
static LONG   reported;
static pthread_once_t first_session = PTHREAD_ONCE_INIT;

static LONG64 pack( struct w32ios_size size )
{
    return (LONG64)((ULONG64)(UINT)size.w << 32 | (UINT)size.h);
}

static struct w32ios_size unpack( LONG64 value )
{
    struct w32ios_size size = { (int)(UINT)((ULONG64)value >> 32), (int)(UINT)value };
    return size;
}

/* the compiler's atomics: Wine's 64-bit Interlocked helpers aren't declared for unix code */
static void store_size( LONG64 *word, LONG64 value )
{
    __atomic_store_n( word, value, __ATOMIC_SEQ_CST );
}

static LONG64 load_size( const LONG64 *word )
{
    return __atomic_load_n( word, __ATOMIC_RELAXED );
}

/* A program with no parent creates its slot (w32ios_proc.c): the app booted it, first or as a
 * later program of a kept session. Clarification 10: its launch's PG_SCREEN_W/H become the
 * monitor's size before it runs, unless another user program of the session still runs. */
void w32ios_display_session_start( void )
{
    const char *src = getenv( "PG_SCREEN_SRC" );
    struct w32ios_size want = w32ios_session_default( getenv( "PG_SCREEN_W" ), getenv( "PG_SCREEN_H" ) );
    LONG64 cur_word = load_size( &current_size );
    struct w32ios_size cur = cur_word ? unpack( cur_word ) : want;
    enum w32ios_launch_size what = w32ios_launch_size( !load_size( &default_size ), cur, want, FALSE );

    /* the process list is read only when the size would change */
    if (what == W32IOS_LAUNCH_CHANGE && w32ios_other_user_program_running()) what = W32IOS_LAUNCH_STAYS;

    switch (what)
    {
    case W32IOS_LAUNCH_FIRST:
        store_size( &default_size, pack( want ) );
        store_size( &current_size, pack( want ) );
        InterlockedExchange( &reported, 0 );
        w32ios_log( "virtual monitor %dx%d, 32 bpp, 60 Hz (PG_SCREEN_SRC=%s)", want.w, want.h,
                    src && *src ? src : "unset" );
        break;
    case W32IOS_LAUNCH_SAME:
        store_size( &default_size, pack( want ) );   /* its modes list is built on its own size */
        w32ios_log_verbose( "virtual monitor %dx%d for a new program (unchanged)", want.w, want.h );
        break;
    case W32IOS_LAUNCH_CHANGE:
        store_size( &default_size, pack( want ) );
        store_size( &current_size, pack( want ) );
        InterlockedExchange( &reported, 0 );
        w32ios_monitor_resized();
        w32ios_log( "virtual monitor %dx%d (was %dx%d) for a new program", want.w, want.h, cur.w, cur.h );
        break;
    case W32IOS_LAUNCH_STAYS:
        w32ios_log( "virtual monitor stays %dx%d: another program is running", cur.w, cur.h );
        break;
    }
}

static void start_first_session( void )
{
    if (!load_size( &default_size )) w32ios_display_session_start();
}

struct w32ios_size w32ios_display_default( void )
{
    LONG64 value = load_size( &default_size );

    if (!value)
    {
        pthread_once( &first_session, start_first_session );
        value = load_size( &default_size );
    }
    return unpack( value );
}

struct w32ios_size w32ios_display_current( void )
{
    LONG64 value = load_size( &current_size );

    if (!value) return w32ios_display_default();
    return unpack( value );
}

void w32ios_display_set_current( struct w32ios_size size )
{
    store_size( &current_size, pack( size ) );
}

BOOL w32ios_display_extended_modes( void )
{
    static int value = -1;
    if (value < 0) value = w32ios_env_bool( "PG_EXTENDED_MODES", FALSE );
    return value;
}

BOOL w32ios_display_mode_set_enabled( void )
{
    static int value = -1;
    if (value < 0) value = w32ios_env_bool( "PG_VIRTUAL_MODE_SET", TRUE );
    return value;
}

BOOL w32ios_display_take_report( void )
{
    return !InterlockedExchange( &reported, 1 );
}

void w32ios_display_fill_devmode( DEVMODEW *devmode, struct w32ios_size size )
{
    devmode->dmFields = DM_POSITION | DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH |
                        DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    devmode->dmPosition.x = 0;
    devmode->dmPosition.y = 0;
    devmode->dmDisplayOrientation = DMDO_DEFAULT;
    devmode->dmDisplayFixedOutput = DMDFO_DEFAULT;
    devmode->dmBitsPerPel = 32;
    devmode->dmPelsWidth = size.w;
    devmode->dmPelsHeight = size.h;
    devmode->dmDisplayFlags = 0;
    devmode->dmDisplayFrequency = 60;
}

void w32ios_app_display_mode_changed( int w, int h )
{
    if (pgios_display_mode_changed) pgios_display_mode_changed( w, h );
}

void w32ios_gpu_identity( UINT16 *vendor, UINT16 *device, const char **name )
{
    static int nvidia = -1;

    if (nvidia < 0) nvidia = w32ios_env_bool( "DXMT_ENABLE_NVEXT", FALSE );
    if (nvidia)
    {
        *vendor = 0x10de;
        *device = 0x2544;
        *name = "NVIDIA GeForce RTX 3060";
    }
    else
    {
        *vendor = 0x106b;
        *device = 0x0001;
        *name = "Apple GPU";
    }
}
