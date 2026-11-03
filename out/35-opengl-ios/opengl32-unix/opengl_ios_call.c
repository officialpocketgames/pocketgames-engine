// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Parts B1, E1-E4: what every opengl32 unix entry does before its upstream entry.
//
// - OpenGL off for the calling process (B1): the three setup entries succeed, every other is
//   STATUS_NOT_SUPPORTED; the parameter block is never touched. The process's answer is cached
//   per thread, so the test is one thread-local read.
// - Call statistics (E1, PG_GL_CALL_STATS=1, read once): a plain 32-bit add on a static table;
//   races may lose counts. Off, an entry pays one test of a flag.
// - The report (E2): at a wglSwapBuffers 10 s or more after the period began (the first counted
//   call, then the last report), one line, then the counts start again.
// - The upstream entry runs inside an autorelease pool (E4, pgl_call_in_pool).

#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "unixlib.h"
#include "opengl_ios.h"
#include "pgl.h"

static _Thread_local signed char thread_on;

int opengl_ios_thread_get( void )
{
    return thread_on;
}

void opengl_ios_thread_set( int on )
{
    thread_on = (signed char)on;
}

/* ---- E ---- */

static int stats_on;
static uint32_t counts[funcs_count];
static uint64_t gate_calls;          /* G3 (a later step) counts the gate's calls here */
static uint64_t period_start_ns;

void opengl_ios_stats_init( int on )
{
    static int done;

    if (done) return;
    done = 1;
    stats_on = on;
    if (on) opengl_ios_log_line( "[opengl] call statistics on (PG_GL_CALL_STATS=1)" );
}

static uint64_t monotonic_ns( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* the clock of the statistics (the host tests replace it) */
uint64_t (*opengl_ios_clock)( void ) = monotonic_ns;

uint64_t opengl_ios_now_ns( void )
{
    return opengl_ios_clock();
}

static void report( uint64_t now )
{
    char line[2048];
    double seconds = (double)(now - period_start_ns) / 1e9;

    opengl_ios_format_report( line, sizeof(line), counts, funcs_count, opengl_ios_entry_names,
                              opengl_ios_swap_index, gate_calls, seconds );
    opengl_ios_log_line( line );
    memset( counts, 0, sizeof(counts) );
    gate_calls = 0;
    period_start_ns = now;
}

static void count( unsigned int index )
{
    counts[index]++;
    if (!period_start_ns) period_start_ns = opengl_ios_now_ns();
    if (index == opengl_ios_swap_index)
    {
        uint64_t now = opengl_ios_now_ns();
        if (opengl_ios_report_due( now, period_start_ns )) report( now );
    }
}

/* ---- E4 ---- */

NTSTATUS opengl_ios_call( unsigned int index, int wow64, void *args )
{
    const unixlib_entry_t *table;
    int on;

    if (index == unix_process_attach) return opengl_ios_process_attach( wow64, args );
    if (!(on = thread_on)) on = opengl_ios_thread_resolve();
    if (on < 0) return opengl_ios_off_call( index );

    if (stats_on) count( index );
    table = wow64 ? opengl32_upstream_wow64_funcs : opengl32_upstream_funcs;
    return pgl_call_in_pool( (int32_t (*)( void * ))table[index], args );
}
