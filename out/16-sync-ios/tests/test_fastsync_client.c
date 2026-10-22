// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part J3: Part D's pure parts. The seqlock cache (publish, evict, process-id flush, -1
// entries, the access check), the spin credit arithmetic, the auto window, the peek periods, the
// remaining-timeout arithmetic and the watchdog slices; plus the client's operations on a cell.

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ios_sync.h"
#include "check.h"

static void sleep_ms( unsigned int ms )
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000l };
    nanosleep( &ts, NULL );
}

static struct ios_fs_entry entry( uint32_t handle, uint32_t pid, uint32_t access, int32_t index, uint32_t gen )
{
    struct ios_fs_entry e = { handle, pid, access, gen, index, 0 };
    return e;
}

static void test_cache(void)
{
    struct ios_fs_entry e, out;

    e = entry( 0x40, 8, 0x1f0003, 5, 77 );
    ios_fs_cache_store( &e, 0 );
    CHECK( ios_fs_cache_lookup( 0x40, 8, &out ));
    CHECK_EQ( out.index, 5 );
    CHECK_EQ( out.gen, 77 );
    CHECK_EQ( out.access, 0x1f0003 );
    CHECK( !ios_fs_cache_lookup( 0x40, 9, &out ));   /* another process */
    CHECK( !ios_fs_cache_lookup( 0x44, 8, &out ));
    /* the same slot, another key: replaces */
    e = entry( 0x40 + IOS_FS_CACHE_ENTRIES * 4, 8, 1, 6, 1 );
    ios_fs_cache_store( &e, 0 );
    CHECK( !ios_fs_cache_lookup( 0x40, 8, &out ));
    CHECK( ios_fs_cache_lookup( 0x40 + IOS_FS_CACHE_ENTRIES * 4, 8, &out ));

    /* -1: known not to be a fast object */
    e = entry( 0x48, 8, 0x100000, IOS_FS_NOT_FAST, 0 );
    ios_fs_cache_store( &e, 0 );
    CHECK( ios_fs_cache_lookup( 0x48, 8, &out ));
    CHECK_EQ( out.index, IOS_FS_NOT_FAST );
    CHECK( !ios_fs_entry_usable( &out, 0x100000 ));

    /* evict by handle value, whatever the process */
    e = entry( 0x4c, 3, 1, 1, 1 );
    ios_fs_cache_store( &e, 0 );
    ios_fs_cache_evict_handle( 0x4c, 0 );
    CHECK( !ios_fs_cache_lookup( 0x4c, 3, &out ));
    ios_fs_cache_evict_handle( 0x4c, 0 );   /* nothing there: fine */

    /* the process-id flush */
    e = entry( 0x50, 12, 1, 1, 1 ); ios_fs_cache_store( &e, 0 );
    e = entry( 0x54, 12, 1, 1, 1 ); ios_fs_cache_store( &e, 1 );
    e = entry( 0x58, 13, 1, 1, 1 ); ios_fs_cache_store( &e, 0 );
    ios_fs_cache_evict_pid( 12, 0 );
    CHECK( !ios_fs_cache_lookup( 0x50, 12, &out ));
    CHECK( !ios_fs_cache_lookup( 0x54, 12, &out ));
    CHECK( ios_fs_cache_lookup( 0x58, 13, &out ));

    /* handle 0 is never cached */
    e = entry( 0, 12, 1, 1, 1 );
    ios_fs_cache_store( &e, 0 );
    CHECK( !ios_fs_cache_lookup( 0, 12, &out ));
}

/* readers never see a torn entry while a writer rewrites the slot */
static volatile int seq_stop;
static long seq_torn, seq_hits;

static void *seq_reader( void *arg )
{
    struct ios_fs_entry out;
    (void)arg;
    while (!seq_stop)
    {
        if (!ios_fs_cache_lookup( 0x100, 1, &out )) continue;
        __atomic_add_fetch( &seq_hits, 1, __ATOMIC_RELAXED );
        if (out.gen != out.access || (uint32_t)out.index != (out.gen & 0xff) || out.manual != (out.gen & 1))
            __atomic_add_fetch( &seq_torn, 1, __ATOMIC_RELAXED );
    }
    return NULL;
}

static void test_seqlock(void)
{
    pthread_t readers[3];
    uint32_t i;
    int t;

    for (t = 0; t < 3; t++) pthread_create( &readers[t], NULL, seq_reader, NULL );
    for (i = 1; i < 200000; i++)
    {
        struct ios_fs_entry e = { 0x100, 1, i, i, (int32_t)(i & 0xff), i & 1 };
        ios_fs_cache_store( &e, 1 );
        if (!(i % 1000)) ios_fs_cache_evict_handle( 0x100, 1 );
    }
    seq_stop = 1;
    for (t = 0; t < 3; t++) pthread_join( readers[t], NULL );
    CHECK_EQ( seq_torn, 0 );
    CHECK( seq_hits > 0 );
}

static void test_reply(void)
{
    int32_t index;
    uint32_t manual;

    CHECK( !ios_fs_reply_index( 5, &index, &manual ));
    CHECK( ios_fs_reply_index( 0x40000000 | 7, &index, &manual ));
    CHECK_EQ( index, 7 );
    CHECK_EQ( manual, 0 );
    CHECK( ios_fs_reply_index( 0x60000000 | (IOS_SYNC_CELLS - 1), &index, &manual ));
    CHECK_EQ( index, IOS_SYNC_CELLS - 1 );
    CHECK_EQ( manual, 1 );
    CHECK( !ios_fs_reply_index( 0x40000000 | IOS_SYNC_CELLS, &index, &manual ));   /* never trusted */
    CHECK( !ios_fs_reply_index( 0x40ffffff, &index, &manual ));
}

static void test_usable(void)
{
    int ev = ios_cell_alloc( IOS_CELL_EVENT, 0, 0, IOS_CELL_RESET );
    int sem = ios_cell_alloc( IOS_CELL_SEMAPHORE, 0, 4, 0 );
    struct ios_fs_entry e = entry( 4, 1, 0x0002, ev, 1 );

    CHECK( ios_fs_entry_usable( &e, 0x0002 ));
    CHECK( !ios_fs_entry_usable( &e, 0x100000 ));   /* the access check */
    e.access = 0x100002;
    CHECK( ios_fs_entry_usable( &e, 0x100000 ));
    e.index = sem;   /* MADEIRA_FASTSYNC_SEM is on in this test */
    CHECK( ios_fs_entry_usable( &e, 0x100000 ));
    ios_cell_free( ev );
    ios_cell_free( sem );
}

static void test_spin_credit(void)
{
    struct ios_fs_spin st = { IOS_FS_CREDIT_MAX, 0 };
    int probe, i, probes = 0;

    CHECK_EQ( ios_fs_spin_budget( &st, 40000, &probe ), 40000 );
    CHECK( !probe );
    ios_fs_spin_result( &st, 1, 0 );
    CHECK_EQ( st.credit, 64 );   /* capped */
    for (i = 0; i < 10; i++) ios_fs_spin_result( &st, 0, 0 );
    CHECK_EQ( st.credit, 54 );
    CHECK_EQ( ios_fs_spin_budget( &st, 64000, &probe ), 54000 );
    ios_fs_spin_result( &st, 1, 0 );
    CHECK_EQ( st.credit, 56 );
    /* about one in three paying keeps it: +2 -1 -1 */
    for (i = 0; i < 300; i++) ios_fs_spin_result( &st, i % 3 == 0, 0 );
    CHECK_EQ( st.credit, 56 );
    for (i = 0; i < 100; i++) ios_fs_spin_result( &st, 0, 0 );
    CHECK_EQ( st.credit, 0 );
    /* at 0: one wait in 64 probes with ceiling / 8 */
    for (i = 0; i < 640; i++)
    {
        uint64_t b = ios_fs_spin_budget( &st, 40000, &probe );
        if (probe) { probes++; CHECK_EQ( b, 5000 ); }
        else CHECK_EQ( b, 0 );
    }
    CHECK_EQ( probes, 10 );
    ios_fs_spin_result( &st, 0, 1 );
    CHECK_EQ( st.credit, 0 );
    ios_fs_spin_result( &st, 1, 1 );   /* a probe that pays */
    CHECK_EQ( st.credit, 16 );
}

static void test_auto_window(void)
{
    struct ios_fs_auto st;
    uint64_t t0 = 5000000000ull, el, inwin, n;
    const uint64_t s = 1000000000ull;

    memset( &st, 0, sizeof(st) );
    CHECK( !ios_fs_auto_check( &st, 256, t0, 1000, &el, &inwin ));
    CHECK( !ios_fs_auto_check( &st, 768, t0 + 1 * s, 1000, &el, &inwin ));
    CHECK( ios_fs_auto_check( &st, 1024, t0 + 9 * s, 1000, &el, &inwin ));   /* inside the window */
    CHECK_EQ( inwin, 1024 );
    CHECK_EQ( el, 9 * s );

    /* a window that ends below the threshold restarts the count */
    memset( &st, 0, sizeof(st) );
    CHECK( !ios_fs_auto_check( &st, 256, t0, 1000, &el, &inwin ));
    CHECK( !ios_fs_auto_check( &st, 768, t0 + 11 * s, 1000, &el, &inwin ));   /* window over */
    CHECK( !ios_fs_auto_check( &st, 1536, t0 + 12 * s, 1000, &el, &inwin ));
    CHECK_EQ( inwin, 768 );
    CHECK( ios_fs_auto_check( &st, 1792, t0 + 13 * s, 1000, &el, &inwin ));
    CHECK_EQ( inwin, 1024 );

    /* a slow trickle (256 every 5 s, under 1000 in any 10 s) never arms */
    memset( &st, 0, sizeof(st) );
    for (n = 256; n < 100000; n += 256)
        CHECK( !ios_fs_auto_check( &st, n, t0 + n / 256 * 5 * s, 1000, &el, &inwin ));
}

static void test_peek_periods(void)
{
    struct ios_fs_peek st = { 0, 0 };
    int i, to_server = 0, yields = 0;

    for (i = 1; i <= 160; i++) if (ios_fs_peek_to_server( &st )) { to_server++; CHECK( i % 16 == 0 ); }
    CHECK_EQ( to_server, 10 );
    /* the two periods are independent */
    for (i = 1; i <= 640; i++) if (ios_fs_peek_answered( &st )) { yields++; CHECK( i % 64 == 0 ); }
    CHECK_EQ( yields, 10 );
    CHECK_EQ( st.polls, 160 );
}

static void test_remaining(void)
{
    /* 1 ms relative timeout = 10000 units of 100 ns */
    CHECK_EQ( ios_fs_remaining_100ns( 10000, 300000, 2000000 ), 7000 );
    CHECK_EQ( ios_fs_remaining_100ns( 10000, 1000000, 2000000 ), 0 );   /* all of it used */
    CHECK_EQ( ios_fs_remaining_100ns( 10000, 5000000, 2000000 ), 0 );   /* never negative */
    /* an overshooting clock is charged at most the cap */
    CHECK_EQ( ios_fs_remaining_100ns( 100000, 50000000, 2000000 ), 80000 );
    CHECK( ios_fs_remaining_100ns( 10000, 999999, 2000000 ) > 0 );   /* 0 only when all was used */
    CHECK_EQ( ios_fs_remaining_100ns( 10000, 999999, 900000 ), 1000 );
}

static void test_watchdog(void)
{
    unsigned int s = 0, expect[] = { 2, 8, 32, 60, 60, 60 }, i;
    for (i = 0; i < 6; i++)
    {
        s = ios_fs_watchdog_next( s );
        CHECK_EQ( s, expect[i] );
    }
}

/* ---- the operations on a cell ---- */

static void test_cell_ops(void)
{
    int ev = ios_cell_alloc( IOS_CELL_EVENT, 0, 0, IOS_CELL_RESET );
    int man = ios_cell_alloc( IOS_CELL_EVENT, 1, 0, IOS_CELL_SET );
    int sem = ios_cell_alloc( IOS_CELL_SEMAPHORE, 0, 3, 1 );
    uint32_t gev = ios_cell_gen( ios_cell_load( ios_cell_at( ev )));
    uint32_t gman = ios_cell_gen( ios_cell_load( ios_cell_at( man )));
    uint32_t gsem = ios_cell_gen( ios_cell_load( ios_cell_at( sem )));
    unsigned int prev = 99, count, max;
    int prev_state, wake;

    /* events */
    CHECK_EQ( ios_fs_cli_try( ev, gev ), 0 );
    CHECK( ios_fs_cli_peek_unsignaled( ev, gev ));
    CHECK_EQ( ios_fs_cli_event_change( ev, gev, 1, &prev_state, &wake ), 0 );
    CHECK_EQ( prev_state, 0 );
    CHECK_EQ( wake, 0 );
    CHECK_EQ( ios_fs_cli_event_change( ev, gev, 1, &prev_state, &wake ), 0 );   /* no transition */
    CHECK_EQ( prev_state, 1 );
    CHECK( !ios_fs_cli_peek_unsignaled( ev, gev ));
    CHECK( ios_fs_cli_reads_signaled( ev, gev ));
    CHECK_EQ( ios_fs_cli_try( ev, gev ), 1 );   /* auto: consumed */
    CHECK_EQ( ios_fs_cli_try( ev, gev ), 0 );
    /* a server waiter asks for a wake request */
    ios_fs_srv_queue( ev, 1 );
    CHECK_EQ( ios_fs_cli_event_change( ev, gev, 1, &prev_state, &wake ), 0 );
    CHECK_EQ( wake, 1 );
    CHECK_EQ( ios_fs_cli_event_change( ev, gev, 0, &prev_state, &wake ), 0 );
    CHECK_EQ( prev_state, 1 );
    CHECK_EQ( wake, 0 );   /* a reset never wakes */
    ios_fs_srv_queue( ev, -1 );
    /* CLAIMED is the server's: the client goes to the server */
    ios_cell_store( ios_cell_at( ev ), ios_cell_make( gev, IOS_CELL_CLAIMED ));
    CHECK_EQ( ios_fs_cli_try( ev, gev ), IOS_FS_GO_SERVER );
    CHECK_EQ( ios_fs_cli_event_change( ev, gev, 1, &prev_state, &wake ), IOS_FS_GO_SERVER );
    CHECK( !ios_fs_cli_reads_signaled( ev, gev ));
    ios_cell_store( ios_cell_at( ev ), ios_cell_make( gev, IOS_CELL_RESET ));
    /* a wrong generation never touches the cell */
    CHECK_EQ( ios_fs_cli_event_change( ev, gev + 1, 1, &prev_state, &wake ), IOS_FS_GO_SERVER );
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( ev ))), IOS_CELL_RESET );
    CHECK( !ios_fs_cli_peek_unsignaled( ev, gev + 1 ));
    /* an event operation on a semaphore cell: the server's type error */
    CHECK_EQ( ios_fs_cli_event_change( sem, gsem, 1, &prev_state, &wake ), IOS_FS_GO_SERVER );

    /* manual */
    CHECK_EQ( ios_fs_cli_try( man, gman ), 1 );
    CHECK_EQ( ios_fs_cli_try( man, gman ), 1 );   /* without consuming */

    /* semaphores */
    CHECK_EQ( ios_fs_cli_sem_release( sem, gsem, 2, &prev, &wake ), 0 );
    CHECK_EQ( prev, 1 );
    prev = 99;
    CHECK_EQ( ios_fs_cli_sem_release( sem, gsem, 1, &prev, &wake ), 1 );   /* limit, nothing changes */
    CHECK_EQ( prev, 99 );
    CHECK_EQ( ios_fs_cli_sem_release( sem, gsem, 0xffffffffu, &prev, &wake ), 1 );
    CHECK_EQ( ios_fs_cli_sem_query( sem, gsem, &count, &max ), 0 );
    CHECK_EQ( count, 3 );
    CHECK_EQ( max, 3 );
    CHECK_EQ( ios_fs_cli_try( sem, gsem ), 1 );
    CHECK_EQ( ios_fs_cli_try( sem, gsem ), 1 );
    CHECK_EQ( ios_fs_cli_try( sem, gsem ), 1 );
    CHECK_EQ( ios_fs_cli_try( sem, gsem ), 0 );
    CHECK( ios_fs_cli_peek_unsignaled( sem, gsem ));
    CHECK_EQ( ios_fs_cli_sem_release( ev, gev, 1, &prev, &wake ), IOS_FS_GO_SERVER );

    /* disabled */
    CHECK_EQ( ios_fs_sem_disable( sem ), 0 );
    CHECK_EQ( ios_fs_cli_try( sem, gsem ), IOS_FS_GO_SERVER );
    CHECK_EQ( ios_fs_cli_sem_release( sem, gsem, 1, &prev, &wake ), IOS_FS_GO_SERVER );
    CHECK_EQ( ios_fs_cli_sem_query( sem, gsem, &count, &max ), IOS_FS_GO_SERVER );

    ios_cell_free( ev );
    ios_cell_free( man );
    ios_cell_free( sem );
}

/* the fast wait: spin, park, woken by a set from another thread */
struct setter_arg
{
    int      cell;
    uint32_t gen;
    unsigned delay_ms;
};

static void *setter_main( void *ptr )
{
    struct setter_arg *a = ptr;
    int prev, wake;

    sleep_ms( a->delay_ms );
    ios_fs_cli_event_change( a->cell, a->gen, 1, &prev, &wake );
    return NULL;
}

static void test_fast_wait(void)
{
    int ev = ios_cell_alloc( IOS_CELL_EVENT, 0, 0, IOS_CELL_RESET );
    uint32_t gen = ios_cell_gen( ios_cell_load( ios_cell_at( ev )));
    struct setter_arg a = { ev, gen, 1 };
    uint64_t spent, cap;
    pthread_t t;

    /* nothing comes: falls through after the park budget (cap 2000 us here) */
    CHECK_EQ( ios_fs_cli_wait( ev, gen, UINT64_MAX, &spent, &cap ), 0 );
    CHECK( spent >= 2000000 );
    CHECK( spent < 1000000000 );
    CHECK_EQ( ios_cell_cli_waiters( ios_cell_at( ev )), 0 );
    /* a short budget limits the park */
    CHECK_EQ( ios_fs_cli_wait( ev, gen, 300000, &spent, &cap ), 0 );
    CHECK( spent >= 300000 );
    CHECK( cap <= 300000 + spent );
    CHECK( ios_fs_remaining_100ns( 3000, spent, cap ) == 0 );

    /* the set wakes the parked waiter, which takes the token */
    pthread_create( &t, NULL, setter_main, &a );
    CHECK_EQ( ios_fs_cli_wait( ev, gen, UINT64_MAX, &spent, &cap ), 1 );
    pthread_join( t, NULL );
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( ev ))), IOS_CELL_RESET );

    /* a recycled cell stops the wait at once */
    ios_cell_free( ev );
    CHECK_EQ( ios_fs_cli_wait( ev, gen, UINT64_MAX, &spent, &cap ), 0 );
    CHECK( spent < 1000000 );
}

/* the D2 gate in auto mode is app-wide: not tested here (mode 1 in this test) */
static void test_gates(void)
{
    CHECK( ios_fs_cells_on() );
    CHECK( ios_fs_armed() );
    ios_fs_process_init( 42, 0 );
    ios_fs_set_other_engine( 1 );
    CHECK( !ios_fs_cells_on() );
    CHECK( !ios_fs_armed() );
    ios_fs_set_other_engine( 0 );
}

int main(void)
{
    setenv( "MADEIRA_FASTSYNC", "1", 1 );
    setenv( "MADEIRA_FASTSYNC_SEM", "1", 1 );
    setenv( "MADEIRA_FASTSYNC_SPIN_US", "40", 1 );
    setenv( "MADEIRA_FASTSYNC_CAP_US", "2000", 1 );

    test_cache();
    test_seqlock();
    test_reply();
    test_usable();
    test_spin_credit();
    test_auto_window();
    test_peek_periods();
    test_remaining();
    test_watchdog();
    test_cell_ops();
    test_fast_wait();
    test_gates();
    return check_done( "test_fastsync_client" );
}
