// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 22 Part T (Part C): host tests of later programs in a kept session.
//   - the per-PEB tables reuse ended processes' entries after 64 and 200 programs (the PEB
//     registry, the live-children table, the thread registry);
//   - the process-end hook list (C6);
//   - the "own exiting flag" rule (C4): a fake lock that upstream skips once "the process" is
//     exiting stays taken for a second process after the first one's exit;
//   - the previous-TEB static kept per process (C8);
//   - the killed-main-thread rule (C3) and the leftover-thread rule and counts (C5).
//
//   cc -std=c11 -Wall -Wextra -pthread -I../ntdll-unix ../ntdll-unix/ios_pure.c
//      ../ntdll-unix/ios_sig_pure.c ../ntdll-unix/ios_session_pure.c test_session_pure.c

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ios_pure.h"
#include "ios_sig_pure.h"
#include "ios_session_pure.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

/* ---------- C7: tables reuse ended processes' entries ---------- */

struct fake_proc
{
    char peb[64];       /* its address is the key, as a PEB's */
    int  exiting;       /* upstream's process_exiting, per pseudo-process */
    void *prev_teb;     /* upstream's static in exit_thread, per pseudo-process */
};

static struct ios_registry registry;

/* one program's life in the tables: registered with a child and a thread, then all removed */
static int run_program( struct fake_proc *p, int n )
{
    char name[32];
    unsigned int child;
    int ok = 1;

    snprintf( name, sizeof(name), "C:\\game%d.exe", n );
    ok &= ios_registry_publish( &registry, p->peb, p ) == 0;
    ok &= ios_registry_lookup( &registry, p->peb ) == p;
    ok &= (child = ios_children_add( name, n )) != 0;
    ok &= ios_threadreg_add( 0x1000u + (uint32_t)n, (uint64_t)n, (uintptr_t)p->peb + 1 ) == 0;
    /* the end: the slots go */
    ios_threadreg_remove( 0x1000u + (uint32_t)n );
    ios_children_remove( child );
    ok &= ios_registry_remove( &registry, p->peb ) == 0;
    return ok;
}

static void test_tables_reuse(void)
{
    static struct fake_proc procs[200];
    char buf[256];
    int i, ok = 1;

    memset( &registry, 0, sizeof(registry) );
    ios_children_reset();
    ios_threadreg_reset();

    /* 64 programs one after another, then 200 more: never full */
    for (i = 0; i < 64; i++) ok &= run_program( &procs[i], i + 1 );
    CHECK( ok );
    CHECK( ios_registry_count( &registry ) == 0 );
    for (i = 0; i < 200; i++) ok &= run_program( &procs[i], 100 + i );
    CHECK( ok );
    CHECK( ios_registry_count( &registry ) == 0 );
    CHECK( ios_children_live( buf, sizeof(buf), -1, 1e6 ) == 0 );
    CHECK( !ios_threadreg_range_busy( 0, UINTPTR_MAX ) );
    CHECK( ios_threadreg_count() <= 1 );   /* one slot, used again and again */

    /* with some ended programs' children still alive (system processes and stragglers), later
     * programs still find room */
    for (i = 0; i < 10; i++) CHECK( ios_children_add( "straggler.exe", i ) != 0 );
    for (i = 0; i < 200; i++) ok &= run_program( &procs[i], 400 + i );
    CHECK( ok );
    CHECK( ios_children_live( NULL, 0, -1, 1e6 ) == 10 );
    ios_children_reset();

    /* a full registry (64 live processes) frees a slot when one ends */
    for (i = 0; i < IOS_REGISTRY_SLOTS; i++) CHECK( ios_registry_publish( &registry, procs[i].peb, &procs[i] ) == 0 );
    CHECK( ios_registry_publish( &registry, procs[IOS_REGISTRY_SLOTS].peb, &procs[IOS_REGISTRY_SLOTS] ) == -1 );
    CHECK( ios_registry_remove( &registry, procs[5].peb ) == 0 );
    CHECK( ios_registry_publish( &registry, procs[IOS_REGISTRY_SLOTS].peb, &procs[IOS_REGISTRY_SLOTS] ) == 0 );
    CHECK( ios_registry_lookup( &registry, procs[5].peb ) == NULL );
    for (i = 0; i <= IOS_REGISTRY_SLOTS; i++) ios_registry_remove( &registry, procs[i].peb );
    CHECK( ios_registry_count( &registry ) == 0 );
}

/* ---------- C6: process-end hooks ---------- */

static const void *hook_peb[4];
static int hook_order[8], hook_calls;

static void hook_a( const void *peb ) { hook_peb[0] = peb; hook_order[hook_calls++] = 'a'; }
static void hook_b( const void *peb ) { hook_peb[1] = peb; hook_order[hook_calls++] = 'b'; }
static void hook_n( const void *peb ) { (void)peb; hook_calls++; }

static void test_hooks(void)
{
    static struct ios_hook_list list;
    static struct ios_hook_list full;
    char peb[8];
    int i;

    CHECK( ios_hook_list_run( &list, peb ) == 0 );
    CHECK( ios_hook_list_add( &list, NULL ) == -1 );
    CHECK( ios_hook_list_add( &list, hook_a ) == 0 );
    CHECK( ios_hook_list_add( &list, hook_b ) == 0 );
    CHECK( ios_hook_list_add( &list, hook_a ) == 1 );    /* once */
    hook_calls = 0;
    CHECK( ios_hook_list_run( &list, peb ) == 2 );
    CHECK( hook_calls == 2 && hook_order[0] == 'a' && hook_order[1] == 'b' );
    CHECK( hook_peb[0] == peb && hook_peb[1] == peb );

    /* at most 16 */
    for (i = 0; i < IOS_PROCESS_END_HOOKS_MAX; i++)
    {
        /* 16 distinct functions: the same one at another address is not available in C, so
         * the list's duplicate rule is bypassed by filling with hook_n after the first */
        if (i == 0) CHECK( ios_hook_list_add( &full, hook_n ) == 0 );
        else { full.fn[i] = hook_n; full.count = i + 1; }
    }
    CHECK( ios_hook_list_add( &full, hook_a ) == -1 );
    hook_calls = 0;
    CHECK( ios_hook_list_run( &full, peb ) == IOS_PROCESS_END_HOOKS_MAX && hook_calls == IOS_PROCESS_END_HOOKS_MAX );
}

/* ---------- C4: the exiting flag is the process's own ---------- */

/* upstream's mutex_lock: no locking once "the process" is exiting. Here "the process" is the
 * calling thread's, found by its PEB (as ios_proc_current does through the registry). */
static int fake_lock_taken;
static void fake_mutex_lock( const void *peb )
{
    struct fake_proc *p = ios_registry_lookup( &registry, peb );
    if (!p || !p->exiting) fake_lock_taken++;
}

static void test_own_exiting_flag(void)
{
    static struct fake_proc first, second;

    memset( &registry, 0, sizeof(registry) );
    CHECK( ios_registry_publish( &registry, first.peb, &first ) == 0 );
    fake_lock_taken = 0;
    fake_mutex_lock( first.peb );
    CHECK( fake_lock_taken == 1 );
    /* the first program exits: its own flag is set, its locks are skipped from then on */
    first.exiting = 1;
    fake_mutex_lock( first.peb );
    CHECK( fake_lock_taken == 1 );
    ios_registry_remove( &registry, first.peb );
    /* a second program: its lock is taken; the first one's flag isn't its own */
    CHECK( ios_registry_publish( &registry, second.peb, &second ) == 0 );
    fake_mutex_lock( second.peb );
    fake_mutex_lock( second.peb );
    CHECK( fake_lock_taken == 3 );
    CHECK( !second.exiting );
    ios_registry_remove( &registry, second.peb );
}

/* ---------- C8: the previous exiting thread, per process ---------- */

static void test_prev_teb(void)
{
    static struct fake_proc a, b;
    char teb_a1, teb_a2, teb_b1;

    /* process A: its first leaving thread finds nothing, its second finds the first's TEB */
    CHECK( ios_prev_teb_exchange( &a.prev_teb, &teb_a1, 1 ) == NULL );
    /* process B (started after A ended, with A's TEB still in A's slot): its first leaving
     * thread must not get A's TEB to free or read */
    CHECK( ios_prev_teb_exchange( &b.prev_teb, &teb_b1, 1 ) == NULL );
    CHECK( ios_prev_teb_exchange( &a.prev_teb, &teb_a2, 1 ) == &teb_a1 );
    /* a last thread (or the session's thread) takes it and leaves nothing */
    CHECK( ios_prev_teb_exchange( &a.prev_teb, &teb_a2, 0 ) == &teb_a2 );
    CHECK( a.prev_teb == NULL );
    CHECK( b.prev_teb == &teb_b1 );
}

/* ---------- C3 and C5 rules ---------- */

static void test_rules(void)
{
    struct ios_leftover_counts c;

    /* killed, main, others, ending */
    CHECK( ios_killed_thread_ends_process( 1, 1, 3, 0 ) == 1 );
    CHECK( ios_killed_thread_ends_process( 0, 1, 3, 0 ) == 0 );   /* left of its own accord */
    CHECK( ios_killed_thread_ends_process( 1, 0, 3, 0 ) == 0 );   /* not the main thread */
    CHECK( ios_killed_thread_ends_process( 1, 1, 0, 0 ) == 0 );   /* the last thread: the normal end */
    CHECK( ios_killed_thread_ends_process( 1, 1, 3, 1 ) == 0 );   /* already being ended */

    /* gone, waiting (read before suspending), in Wine */
    CHECK( ios_leftover_action( 1, 0, 0 ) == IOS_LEFTOVER_GONE );
    CHECK( ios_leftover_action( 0, 1, 0 ) == IOS_LEFTOVER_SUSPEND );
    CHECK( ios_leftover_action( 0, 1, 1 ) == IOS_LEFTOVER_LOCKED );
    CHECK( ios_leftover_action( 0, 0, 0 ) == IOS_LEFTOVER_RUNNING );
    CHECK( ios_leftover_action( 0, 0, 1 ) == IOS_LEFTOVER_RUNNING );

    memset( &c, 0, sizeof(c) );
    ios_leftover_count( &c, IOS_LEFTOVER_SUSPEND );
    ios_leftover_count( &c, IOS_LEFTOVER_SUSPEND );
    ios_leftover_count( &c, IOS_LEFTOVER_RUNNING );
    ios_leftover_count( &c, IOS_LEFTOVER_LOCKED );
    ios_leftover_count( &c, IOS_LEFTOVER_GONE );
    CHECK( c.suspended == 2 && c.kept == 2 && c.left == 1 );
}

int main(void)
{
    test_tables_reuse();
    test_hooks();
    test_own_exiting_flag();
    test_prev_teb();
    test_rules();
    printf( "%d checks, %d failed\n", checks, failures );
    return failures ? 1 : 0;
}
