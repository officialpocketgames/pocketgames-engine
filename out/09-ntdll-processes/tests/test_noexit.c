// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Parts A and C2: a failure during the boot ends the pseudo-process and comes back to
// the app by longjmp, with a log line; nothing reaches libc's exit, _exit or abort.
//
// Built (run-host-tests.sh) with -DWINE_IOS and -include ios_noexit.h, as build-ios.sh builds
// ntdll's unix files, and with this folder's assert.h first on the include path. The boot
// stages below call exit(), _exit(), abort() and assert() the way upstream's fatal paths do.
// ios_unix_exit and ios_assert_fail are stand-ins for ios_exit.c's (which needs Wine's
// headers): they log and leave the way ios_exit.c does, through ios_exit_route (ios_pure.c),
// so the routing the device uses is the routing tested here.

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "ios_pure.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

/* ---------- the stand-ins: the session thread's ways out ---------- */

static char log_text[4096];
static jmp_buf app_buf, own_buf, thread_exit_buf;
static int app_ready, own_ready, left_by_pthread_exit, exit_code;

static void log_line( const char *line )
{
    strncat( log_text, line, sizeof(log_text) - strlen( log_text ) - 1 );
}

void ios_unix_exit( int status, const char *why )
{
    char line[256];

    snprintf( line, sizeof(line), "%s(%d): ending the pseudo-process\n", why, status );
    log_line( line );
    exit_code = status;
    switch (ios_exit_route( app_ready, own_ready ))
    {
    case IOS_EXIT_APP_JMP: longjmp( app_buf, 1 );
    case IOS_EXIT_OWN_JMP: longjmp( own_buf, 1 );
    default:
        left_by_pthread_exit = 1;   /* the device would pthread_exit here */
        longjmp( thread_exit_buf, 1 );
    }
}

void ios_assert_fail( const char *expr, const char *file, int line )
{
    char text[256];

    snprintf( text, sizeof(text), "assertion failed: %s (%s:%d)\n", expr, file, line );
    log_line( text );
    ios_unix_exit( IOS_ABORT_EXIT_CODE, "assert" );
}

/* ---------- boot stages that fail as upstream's code does ---------- */

static void stage_fatal_exit(void)   { exit( 1 ); }                     /* fatal_error() */
static void stage_fatal_underscore(void) { _exit( 5 ); }
static void stage_abort(void)        { abort(); }
static void stage_assert(void)       { int status = 7; assert( status == 0 ); }
static void stage_fine(void)         { assert( 1 + 1 == 2 ); }

/* runs a stage the way __wine_main runs the boot: inside its own buffer, maybe the app's too.
 * Returns how it came back: 'a' app buffer, 'o' own buffer, 't' pthread_exit, 'r' returned. */
static char run_stage( void (*stage)(void), int with_app, int with_own )
{
    volatile char how = 'r';

    app_ready = with_app;
    own_ready = with_own;
    left_by_pthread_exit = 0;
    if (setjmp( app_buf )) how = 'a';
    else if (setjmp( own_buf )) how = 'o';
    else if (setjmp( thread_exit_buf )) how = 't';
    else stage();
    return how;
}

static void test_stages(void)
{
    /* the app's buffer isn't set for this thread: __wine_main's own buffer brings it back */
    log_text[0] = 0;
    CHECK( run_stage( stage_fatal_exit, 0, 1 ) == 'o' );
    CHECK( exit_code == 1 );
    CHECK( strstr( log_text, "exit(1): ending the pseudo-process" ) != NULL );

    log_text[0] = 0;
    CHECK( run_stage( stage_fatal_underscore, 0, 1 ) == 'o' );
    CHECK( exit_code == 5 && strstr( log_text, "_exit(5)" ) != NULL );

    log_text[0] = 0;
    CHECK( run_stage( stage_abort, 0, 1 ) == 'o' );
    CHECK( exit_code == IOS_ABORT_EXIT_CODE && strstr( log_text, "abort(3)" ) != NULL );

    log_text[0] = 0;
    CHECK( run_stage( stage_assert, 0, 1 ) == 'o' );
    CHECK( exit_code == IOS_ABORT_EXIT_CODE );
    CHECK( strstr( log_text, "assertion failed: status == 0" ) != NULL );
    CHECK( strstr( log_text, "assert(3)" ) != NULL );

    /* the app's buffer, when set for this thread, wins */
    log_text[0] = 0;
    CHECK( run_stage( stage_fatal_exit, 1, 1 ) == 'a' );
    CHECK( run_stage( stage_fatal_exit, 1, 0 ) == 'a' );

    /* a passing assert does nothing */
    CHECK( run_stage( stage_fine, 0, 1 ) == 'r' );

    /* only a thread that is neither (not the one that called __wine_main) leaves by
     * pthread_exit */
    CHECK( run_stage( stage_fatal_exit, 0, 0 ) == 't' && left_by_pthread_exit );
}

static void test_routes(void)
{
    CHECK( ios_exit_route( 1, 1 ) == IOS_EXIT_APP_JMP );
    CHECK( ios_exit_route( 1, 0 ) == IOS_EXIT_APP_JMP );
    CHECK( ios_exit_route( 0, 1 ) == IOS_EXIT_OWN_JMP );
    CHECK( ios_exit_route( 0, 0 ) == IOS_EXIT_THREAD );
}

/* a set-up left by a jump is reported as failed, never waited for */
static void test_init_state(void)
{
    static jmp_buf out;
    int state = IOS_INIT_NONE;

    CHECK( ios_init_begin( &state ) == IOS_INIT_RUN );
    ios_init_end( &state );
    CHECK( ios_init_begin( &state ) == IOS_INIT_SKIP );
    CHECK( ios_init_begin( &state ) == IOS_INIT_SKIP );

    state = IOS_INIT_NONE;
    if (!setjmp( out ))
    {
        CHECK( ios_init_begin( &state ) == IOS_INIT_RUN );
        longjmp( out, 1 );   /* a fatal error inside the set-up */
    }
    CHECK( ios_init_begin( &state ) == IOS_INIT_FAILED_BEFORE );
    CHECK( ios_init_begin( &state ) == IOS_INIT_FAILED_BEFORE );
}

int main(void)
{
    test_routes();
    test_stages();
    test_init_state();
    printf( "%d checks, %d failed\n", checks, failures );
    return failures ? 1 : 0;
}
