// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part J1: host test of opengl32's iOS wrappers, built from opengl_ios_tables.c and
// opengl_ios_call.c against Wine 11.4's opengl32 unixlib.h (run-host-tests.sh writes the entry
// list with gen-entries.sh first). opengl_ios.c's process decisions, the upstream tables and pgl's
// pool are fakes here. Checks:
//   - both tables have the enum's size and no NULL; the names table; the swap index;
//   - B1 (spec 12 clarification 8's test, kept): with OpenGL off, the three setup entries return
//     0 and every other entry STATUS_NOT_SUPPORTED, in both tables, with the parameter block on a
//     guard page (a touch ends the test with SIGSEGV), and nothing upstream runs;
//   - with OpenGL on, every entry reaches its upstream entry inside the pool, with its block;
//   - E1/E2: the counts, the report at a swap 10 s after the first counted call, the line's
//     form and top list, the reset; the report's format on its own (16 busiest, ties).

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "unixlib.h"
#include "opengl_ios.h"
#include "pgl.h"

static int checks, failures;

#define CHECK( cond ) do { checks++; if (!(cond)) { failures++; \
    printf( "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); } } while (0)
#define STARTS( s, lit ) (!strncmp( (s), (lit), strlen( lit ) ))

/* ---- fakes ---- */

static unsigned int upstream_calls[2], pools;
static void *upstream_args;

static NTSTATUS fake_upstream64( void *args ) { upstream_calls[0]++; upstream_args = args; return 0x1234; }
static NTSTATUS fake_upstream32( void *args ) { upstream_calls[1]++; upstream_args = args; return 0x4321; }

const unixlib_entry_t opengl32_upstream_funcs[funcs_count] = { [0 ... funcs_count - 1] = fake_upstream64 };
const unixlib_entry_t opengl32_upstream_wow64_funcs[funcs_count] = { [0 ... funcs_count - 1] = fake_upstream32 };
const int opengl32_upstream_present = 1;

int32_t pgl_call_in_pool( int32_t (*fn)( void *args ), void *args )
{
    pools++;
    return fn( args );
}

static char last_line[4096];
static unsigned int lines;

void opengl_ios_log_line( const char *line )
{
    snprintf( last_line, sizeof(last_line), "%s", line );
    lines++;
}

static int attach_on, resolve_on;

NTSTATUS opengl_ios_process_attach( int wow64, void *args )
{
    opengl_ios_thread_set( attach_on ? 1 : -1 );
    if (!attach_on) return STATUS_SUCCESS;
    return pgl_call_in_pool( (int32_t (*)( void * ))(wow64 ? opengl32_upstream_wow64_funcs : opengl32_upstream_funcs)[0], args );
}

int opengl_ios_thread_resolve( void )
{
    opengl_ios_thread_set( resolve_on );
    return resolve_on;
}

static unsigned int off_calls;

NTSTATUS opengl_ios_off_call( unsigned int index )
{
    off_calls++;
    return opengl_ios_off_status( index );
}

extern uint64_t (*opengl_ios_clock)( void );
static uint64_t fake_now;
static uint64_t fake_clock( void ) { return fake_now; }

#define SEC 1000000000ull

/* ---- tests ---- */

static void test_tables( void )
{
    unsigned int i, missing = 0;

    CHECK( opengl_ios_entry_count == funcs_count );
    for (i = 0; i < funcs_count; i++)
        if (!opengl32_unix_call_funcs[i] || !opengl32_unix_call_wow64_funcs[i] || !opengl_ios_entry_names[i]) missing++;
    CHECK( missing == 0 );
    CHECK( !strcmp( opengl_ios_entry_names[unix_process_attach], "process_attach" ) );
    CHECK( !strcmp( opengl_ios_entry_names[unix_get_pixel_formats], "get_pixel_formats" ) );
    CHECK( !strcmp( opengl_ios_entry_names[unix_glVertex3f], "glVertex3f" ) );
    CHECK( !strcmp( opengl_ios_entry_names[unix_wglSwapIntervalEXT], "wglSwapIntervalEXT" ) );
    CHECK( opengl_ios_swap_index == unix_wglSwapBuffers );
}

static void test_off( const unixlib_entry_t *table, void *guard )
{
    unsigned int i, wrong = 0, before = off_calls;

    attach_on = 0;
    upstream_calls[0] = upstream_calls[1] = 0;
    CHECK( table[unix_process_attach]( guard ) == STATUS_SUCCESS );
    CHECK( opengl_ios_thread_get() == -1 );
    CHECK( table[unix_thread_attach]( guard ) == STATUS_SUCCESS );
    CHECK( table[unix_process_detach]( guard ) == STATUS_SUCCESS );
    CHECK( table[unix_process_attach]( NULL ) == STATUS_SUCCESS );
    before = off_calls;
    for (i = 0; i < funcs_count; i++)
    {
        NTSTATUS want = i <= unix_process_detach ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
        if (table[i]( guard ) != want) wrong++;
    }
    CHECK( wrong == 0 );
    CHECK( off_calls - before == funcs_count - 1 );   /* every entry but process_attach */
    CHECK( upstream_calls[0] == 0 && upstream_calls[1] == 0 );
    /* a sample by name */
    CHECK( table[unix_wglCreateContext]( guard ) == STATUS_NOT_SUPPORTED );
    CHECK( table[unix_get_pixel_formats]( guard ) == STATUS_NOT_SUPPORTED );
    CHECK( table[unix_wglSwapIntervalEXT]( guard ) == STATUS_NOT_SUPPORTED );

    /* a thread whose process it doesn't know yet asks once */
    opengl_ios_thread_set( 0 );
    resolve_on = -1;
    CHECK( table[unix_glBegin]( guard ) == STATUS_NOT_SUPPORTED );
    CHECK( opengl_ios_thread_get() == -1 );
}

static void test_on( void )
{
    char block[64];
    unsigned int i, wrong = 0;

    attach_on = 1;
    pools = upstream_calls[0] = upstream_calls[1] = 0;
    CHECK( opengl32_unix_call_funcs[unix_process_attach]( block ) == 0x1234 );
    CHECK( opengl_ios_thread_get() == 1 );
    CHECK( pools == 1 && upstream_args == block );
    for (i = 1; i < funcs_count; i++)
    {
        upstream_args = NULL;
        if (opengl32_unix_call_funcs[i]( block + 1 ) != 0x1234 || upstream_args != block + 1) wrong++;
        if (opengl32_unix_call_wow64_funcs[i]( block + 2 ) != 0x4321 || upstream_args != block + 2) wrong++;
    }
    CHECK( wrong == 0 );
    CHECK( upstream_calls[0] == funcs_count && upstream_calls[1] == funcs_count - 1 );
    CHECK( pools == 1 + 2 * (funcs_count - 1) );

    /* a thread of a process with OpenGL on, not known to the thread yet */
    opengl_ios_thread_set( 0 );
    resolve_on = 1;
    CHECK( opengl32_unix_call_funcs[unix_glEnd]( block ) == 0x1234 );
    CHECK( opengl_ios_thread_get() == 1 );
}

static void test_stats( void )
{
    char block[16];
    unsigned int i, before;

    opengl_ios_clock = fake_clock;
    attach_on = 1;
    opengl32_unix_call_funcs[unix_process_attach]( block );
    opengl_ios_stats_init( 1 );
    CHECK( strstr( last_line, "call statistics on" ) != NULL );

    fake_now = 1 * SEC;
    for (i = 0; i < 10; i++) opengl32_unix_call_wow64_funcs[unix_glVertex3f]( block );
    for (i = 0; i < 2; i++) opengl32_unix_call_funcs[unix_glBegin]( block );
    before = lines;
    fake_now = 5 * SEC;
    opengl32_unix_call_funcs[unix_wglSwapBuffers]( block );
    CHECK( lines == before );                        /* 4 s: no report */
    fake_now = 11 * SEC + SEC / 2;
    opengl32_unix_call_funcs[unix_wglSwapBuffers]( block );
    CHECK( lines == before + 1 );                    /* 10.5 s after the first counted call */
    CHECK( STARTS( last_line, "[opengl] calls: 1/s, 7.0/swap (0.2/s), via gate 0/s; top: " ) );
    CHECK( strstr( last_line, "top: glVertex3f 1/s (5.0/swap), wglSwapBuffers 0/s (1.0/swap), glBegin 0/s (1.0/swap)" ) != NULL );

    /* reset: the next period starts at the report */
    fake_now = 12 * SEC;
    opengl32_unix_call_funcs[unix_wglSwapBuffers]( block );
    CHECK( lines == before + 1 );
    fake_now = 21 * SEC + SEC / 2;
    opengl32_unix_call_funcs[unix_wglSwapBuffers]( block );
    CHECK( lines == before + 2 );
    CHECK( STARTS( last_line, "[opengl] calls: 0/s, 1.0/swap (0.2/s), via gate 0/s; top: wglSwapBuffers 0/s (1.0/swap)" ) );
    CHECK( !strstr( last_line, "glVertex3f" ) );

    /* off for a thread: nothing counted */
    opengl_ios_thread_set( -1 );
    fake_now = 40 * SEC;
    opengl32_unix_call_funcs[unix_wglSwapBuffers]( block );
    CHECK( lines == before + 2 );
}

static void test_format( void )
{
    static const char *const names[20] =
        { "e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8", "e9", "e10", "e11", "e12", "e13", "e14", "e15", "e16", "e17", "e18", "swap" };
    uint32_t counts[20];
    char line[1024];
    unsigned int i;

    for (i = 0; i < 19; i++) counts[i] = 100 + i;   /* e18 busiest, e0 and e1 last */
    counts[19] = 10;
    counts[5] = 118;                                 /* ties with e18: e5 (lower index) first */
    opengl_ios_format_report( line, sizeof(line), counts, 20, names, 19, 20, 10.0 );
    CHECK( STARTS( line, "[opengl] calls: 209/s, 209.4/swap (1.0/s), via gate 2/s; top: " ) );
    CHECK( strstr( line, "via gate 2/s" ) != NULL );
    CHECK( strstr( line, "top: e5 12/s (11.8/swap), e18 12/s (11.8/swap), e17 12/s" ) != NULL );
    CHECK( !strstr( line, " e0 " ) && !strstr( line, " e1 " ) && !strstr( line, " e2 " ) );
    CHECK( strstr( line, " e3 " ) != NULL );          /* the 16th */

    CHECK( !opengl_ios_report_due( 5 * SEC, 0 ) );
    CHECK( !opengl_ios_report_due( 11 * SEC - 1, 1 * SEC ) );
    CHECK( opengl_ios_report_due( 11 * SEC, 1 * SEC ) );
    CHECK( opengl_ios_switch_on( "1" ) && !opengl_ios_switch_on( "0" ) && !opengl_ios_switch_on( "" ) &&
           !opengl_ios_switch_on( NULL ) && !opengl_ios_switch_on( "11" ) && !opengl_ios_switch_on( "on" ) );

    /* a tiny buffer stays terminated */
    CHECK( opengl_ios_format_report( line, 8, counts, 20, names, 19, 0, 1.0 ) == 7 && strlen( line ) == 7 );
}

int main( void )
{
    long page = sysconf( _SC_PAGESIZE );
    char *guard = mmap( NULL, 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );

    CHECK( guard != MAP_FAILED );
    test_tables();
    if (guard != MAP_FAILED)
    {
        test_off( opengl32_unix_call_funcs, guard + page / 2 );
        test_off( opengl32_unix_call_wow64_funcs, guard + page );
    }
    test_on();
    test_stats();
    test_format();
    printf( "test_opengl_ios: %d checks, %d failures (%u entries)\n", checks, failures, (unsigned int)funcs_count );
    return failures != 0;
}
