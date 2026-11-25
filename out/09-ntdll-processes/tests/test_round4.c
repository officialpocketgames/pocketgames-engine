// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 4: host tests of ios_lock.c (real threads) and ios_diag_pure.c.

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ios_lock.h"
#include "ios_diag_pure.h"

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf( stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond ); } } while (0)

/* ---------- fakes: who is asking, and the log ---------- */

static __thread struct ios_lock_who me;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char log_buf[8192];

static void fake_who( struct ios_lock_who *who ) { *who = me; }

static void fake_log( const char *line )
{
    pthread_mutex_lock( &log_mutex );
    if (strlen( log_buf ) + strlen( line ) < sizeof(log_buf)) strcat( log_buf, line );
    pthread_mutex_unlock( &log_mutex );
}

static int log_has( const char *text )
{
    int ret;

    pthread_mutex_lock( &log_mutex );
    ret = strstr( log_buf, text ) != NULL;
    pthread_mutex_unlock( &log_mutex );
    return ret;
}

static void log_clear( void )
{
    pthread_mutex_lock( &log_mutex );
    log_buf[0] = 0;
    pthread_mutex_unlock( &log_mutex );
}

static unsigned long long ms_now( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_me( unsigned int tid, unsigned int pid, unsigned int serial )
{
    me.tid = tid;
    me.pid = pid;
    me.serial = serial;
}

/* ---------- 1.1: a thread killed inside a fake unix call finishes it first ---------- */

static pthread_mutex_t call_lock = PTHREAD_MUTEX_INITIALIZER;
static int call_finished, lock_free_at_exit, left_at_boundary;

#define FAKE_TERMINATING 0xc000004b   /* STATUS_THREAD_IS_TERMINATING */

/* a server request whose pipe the server has closed */
static unsigned int fake_server_call( int *killed )
{
    if (ios_kill_on_server_gone( 1, 0, 0 ) == IOS_KILL_FAIL)
    {
        *killed = 1;
        return FAKE_TERMINATING;
    }
    pthread_exit( NULL );   /* the old way: leave right here, the lock still held */
}

/* a unix call that takes a shared lock and asks the server inside it */
static unsigned int fake_unix_call( int *killed )
{
    unsigned int status;

    ios_lock_take( &call_lock, "fake call lock" );
    status = fake_server_call( killed );
    ios_lock_give( &call_lock );
    call_finished = 1;
    return status;
}

static void *killed_thread( void *arg )
{
    int killed = 0;

    (void)arg;
    set_me( 0x31, 0x40, 3 );
    CHECK( fake_unix_call( &killed ) == FAKE_TERMINATING );
    /* the dispatcher's boundary: the call is over */
    if (ios_kill_leave_at_boundary( killed, 0, 0 ))
    {
        lock_free_at_exit = !pthread_mutex_trylock( &call_lock );
        if (lock_free_at_exit) pthread_mutex_unlock( &call_lock );
        left_at_boundary = 1;
        pthread_exit( NULL );
    }
    return NULL;
}

static void test_kill_finishes_call( void )
{
    pthread_t t;

    pthread_create( &t, NULL, killed_thread, NULL );
    pthread_join( t, NULL );
    CHECK( call_finished );
    CHECK( left_at_boundary );
    CHECK( lock_free_at_exit );
    CHECK( !pthread_mutex_trylock( &call_lock ) );
    pthread_mutex_unlock( &call_lock );

    /* the rule's other answers */
    CHECK( ios_kill_on_server_gone( 1, 0, 1 ) == IOS_KILL_LEAVE );   /* at a wait for the server */
    CHECK( ios_kill_on_server_gone( 1, 1, 0 ) == IOS_KILL_LEAVE );   /* already leaving */
    CHECK( ios_kill_on_server_gone( 0, 0, 0 ) == IOS_KILL_LEAVE );   /* no Windows thread */
    CHECK( !ios_kill_leave_at_boundary( 0, 0, 0 ) );                 /* not killed */
    CHECK( !ios_kill_leave_at_boundary( 1, 1, 0 ) );                 /* already leaving */
    CHECK( !ios_kill_leave_at_boundary( 1, 0, 1 ) );                 /* inside a user callback */
}

/* ---------- 1.2-1.3: a lock held by an ended process, by a live thread ---------- */

static pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int holder_may_go, waiter_has_it;

static void *dying_holder( void *arg )
{
    (void)arg;
    set_me( 0x11, 0x20, 7 );
    ios_lock_take( &shared_lock, "test shared lock" );
    return NULL;   /* ends holding it */
}

static void test_lock_ended_process( void )
{
    pthread_t t;
    unsigned long long start;

    log_clear();
    pthread_create( &t, NULL, dying_holder, NULL );
    pthread_join( t, NULL );
    ios_lock_process_ended( 7 );
    set_me( 0x12, 0x24, 8 );
    start = ms_now();
    ios_lock_take( &shared_lock, NULL );
    CHECK( ms_now() - start < 2000 );
    CHECK( log_has( "[lock] test shared lock waited" ) );
    CHECK( log_has( "by thread 0012 of process 0024; held by thread 0011 of process 0020 (ended)" ) );
    CHECK( log_has( "[lock] test shared lock was held by an ended process: released\n" ) );
    ios_lock_give( &shared_lock );
    /* free again for anyone */
    CHECK( !pthread_mutex_trylock( &shared_lock ) );
    pthread_mutex_unlock( &shared_lock );
}

static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;

static void *live_holder( void *arg )
{
    (void)arg;
    set_me( 0x51, 0x60, 9 );
    ios_lock_take( &live_lock, "test live lock" );
    while (!holder_may_go) usleep( 1000 );
    ios_lock_give( &live_lock );
    return NULL;
}

static void *live_waiter( void *arg )
{
    (void)arg;
    set_me( 0x52, 0x64, 10 );
    ios_lock_take( &live_lock, NULL );
    waiter_has_it = 1;
    ios_lock_give( &live_lock );
    return NULL;
}

static void test_lock_live_holder( int suspend )
{
    pthread_t h, w;

    log_clear();
    holder_may_go = waiter_has_it = 0;
    pthread_create( &h, NULL, live_holder, NULL );
    usleep( 20000 );
    if (suspend) ios_lock_thread_suspended( 9, 0x51 );
    pthread_create( &w, NULL, live_waiter, NULL );
    usleep( 300000 );   /* six times the log time */
    CHECK( log_has( "[lock] test live lock waited" ) );
    CHECK( log_has( suspend ? "held by thread 0051 of process 0060 (suspended)"
                            : "held by thread 0051 of process 0060 (running)" ) );
    CHECK( !log_has( "released" ) );
    CHECK( !waiter_has_it );   /* reported, never taken */
    holder_may_go = 1;
    pthread_join( h, NULL );
    pthread_join( w, NULL );
    CHECK( waiter_has_it );
}

static void test_lock_recursive( void )
{
    pthread_mutex_t rec;
    pthread_mutexattr_t attr;

    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    pthread_mutex_init( &rec, &attr );
    set_me( 0x71, 0x80, 11 );
    ios_lock_take( &rec, "test recursive lock" );
    ios_lock_take( &rec, NULL );
    ios_lock_give( &rec );
    ios_lock_give( &rec );
    CHECK( !pthread_mutex_trylock( &rec ) );
    pthread_mutex_unlock( &rec );
}

/* ---------- 2: the fault line names the call ---------- */

static void test_fault_line( void )
{
    const uint64_t args[4] = { 0x0, 0x7ff6a0001000ull, 0x8, 0x1 };
    char line[512];

    ios_format_syscall_fault( line, sizeof(line), 2, "game.exe", 0x1c4, 0xc0000005, "NtGdiOpenDCW", 0x1078,
                              args, 0x10a3c4f20ull, "pthread_mutex_lock", 0x24, 0x8, 3 );
    CHECK( !strcmp( line, "process 2 (game.exe), thread 01c4: exception c0000005 in system call NtGdiOpenDCW "
                          "(0x1078, args 0 7ff6a0001000 8 1) at pc 0x10a3c4f20 (pthread_mutex_lock+0x24), "
                          "address 0x8: the call returns it as its status (fault 3 of this run)\n" ) );
    ios_format_syscall_fault( line, sizeof(line), 2, "game.exe", 0x1c4, 0xc0000005, NULL, 0x55, args,
                              0x1000, NULL, 0, 0x8, 4 );
    CHECK( strstr( line, "in system call 0x55 (0x55, args 0 7ff6a0001000 8 1) at pc 0x1000, address 0x8:" ) != NULL );
    ios_format_syscall_fault( line, sizeof(line), 2, "game.exe", 0x1c4, 0xc0000005, NULL, IOS_UNIX_CALL_ID,
                              args, 0x1000, NULL, 0, 0x8, 5 );
    CHECK( strstr( line, "in a unix call (args 0 7ff6a0001000 8 1) at pc 0x1000," ) != NULL );
}

/* ---------- 3: the lookup counter and the top three ---------- */

static struct ios_lookup_stats stats;

static void test_lookups( void )
{
    char line[1024];
    int i, top[3], wrote = 0;
    uint64_t t = 1000;

    /* 2001 lookups in 10 s: logged when the next window starts */
    memset( &stats, 0, sizeof(stats) );
    for (i = 0; i < 2001; i++)
    {
        const char *path = (i % 10 < 6) ? "\\??\\C:\\game\\data\\a.pak" :
                           (i % 10 < 9) ? "\\??\\C:\\game\\data\\b.pak" : "\\??\\C:\\game\\c.ini";
        wrote += ios_lookup_note( &stats, t + i * 4, path, i % 10 < 6 ? 0xc0000034 : 0, line, sizeof(line) );
    }
    CHECK( !wrote );
    CHECK( ios_lookup_top3( &stats, top ) == 3 );
    CHECK( !strcmp( stats.paths[top[0]].path, "\\??\\C:\\game\\data\\a.pak" ) );
    CHECK( !strcmp( stats.paths[top[1]].path, "\\??\\C:\\game\\data\\b.pak" ) );
    CHECK( !strcmp( stats.paths[top[2]].path, "\\??\\C:\\game\\c.ini" ) );
    CHECK( ios_lookup_note( &stats, t + 10000, "\\??\\C:\\x", 0, line, sizeof(line) ) == 1 );
    CHECK( !strcmp( line, "2001 lookups in 10 s; most often: \\??\\C:\\game\\data\\a.pak (1201 times, 0xc0000034), "
                          "\\??\\C:\\game\\data\\b.pak (600 times, 0), \\??\\C:\\game\\c.ini (200 times, 0)" ) );
    CHECK( stats.count == 1 );   /* the new window holds the lookup that started it */

    /* 2000 is not more than 2000 */
    memset( &stats, 0, sizeof(stats) );
    for (i = 0; i < 2000; i++) ios_lookup_note( &stats, t + i, "\\??\\C:\\a", 0, line, sizeof(line) );
    CHECK( !ios_lookup_note( &stats, t + 20000, "\\??\\C:\\a", 0, line, sizeof(line) ) );

    /* more paths than the table holds: the loop's paths stay on top */
    memset( &stats, 0, sizeof(stats) );
    for (i = 0; i < 3000; i++)
    {
        char path[32];

        if (i % 3 == 2) snprintf( path, sizeof(path), "\\??\\C:\\once%d", i );
        else snprintf( path, sizeof(path), "\\??\\C:\\loop%d", i % 3 );
        ios_lookup_note( &stats, t + i, path, 0, line, sizeof(line) );
    }
    CHECK( ios_lookup_top3( &stats, top ) == 3 );
    CHECK( !strcmp( stats.paths[top[0]].path, "\\??\\C:\\loop0" ) && stats.paths[top[0]].count == 1000 );
    CHECK( !strcmp( stats.paths[top[1]].path, "\\??\\C:\\loop1" ) && stats.paths[top[1]].count == 1000 );

    /* fewer than three paths */
    memset( &stats, 0, sizeof(stats) );
    ios_lookup_note( &stats, t, "\\??\\C:\\only", 0, line, sizeof(line) );
    CHECK( ios_lookup_top3( &stats, top ) == 1 && top[0] == 0 );
}

int main( void )
{
    ios_lock_setup( fake_who, fake_log, 50 );
    test_kill_finishes_call();
    test_lock_ended_process();
    test_lock_live_holder( 0 );
    test_lock_live_holder( 1 );
    test_lock_recursive();
    test_fault_line();
    test_lookups();
    printf( "%d checks, %d failed\n", checks, failures );
    return failures != 0;
}
