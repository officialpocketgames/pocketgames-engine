// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part B: the one definition of the shared cell table, the park and wake primitives on a
// cell's state half (B5), and the server's allocator (C1). Compiled into libntdll_unix.a; the
// server, a thread of the same app, links against the same copy.

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ios_fastsync.h"

#if defined(__APPLE__)
# include <AvailabilityMacros.h>
# ifdef MAC_OS_VERSION_14_4
#  include <os/os_sync_wait_on_address.h>
# endif
#elif defined(__linux__)
# include <linux/futex.h>
# include <sys/syscall.h>
#endif

struct ios_sync_cell ios_sync_cells[IOS_SYNC_CELLS];

/* ---- logging ---- */

void ios_sync_log( const char *fmt, ... )
{
    char buf[512];
    va_list args;
    int len;

    memcpy( buf, "sync: ", 6 );
    va_start( args, fmt );
    len = vsnprintf( buf + 6, sizeof(buf) - 7, fmt, args );
    va_end( args );
    if (len < 0) return;
    len += 6;
    if (len > (int)sizeof(buf) - 2) len = sizeof(buf) - 2;
    buf[len++] = '\n';
    /* one write per line: no stdio lock, so callers may have signals blocked or hold locks */
    if (write( 2, buf, len ) < 0) return;
}

/* ---- B5: park and wake ---- */

#if defined(__APPLE__)

/* Below iOS 17.4 there is no public address wait (spec 00 B1 rules out upstream's
 * __ulock_wait / __ulock_wake fallback, which the SDK doesn't declare): the park re-reads the
 * word every IOS_PARK_POLL_NS until it changes or the time is up, and a wake has nothing to do.
 * The park's callers already treat every return as "look again". */
#define IOS_PARK_POLL_NS  50000u

int ios_cell_park( struct ios_sync_cell *cell, int32_t state, uint64_t timeout_ns )
{
    uint64_t now, deadline;

    if (!timeout_ns)
    {
        errno = ETIMEDOUT;
        return -1;
    }
#ifdef MAC_OS_VERSION_14_4
    if (__builtin_available( macOS 14.4, iOS 17.4, * ))
    {
        /* the state half: offset 0 on little-endian */
        int ret = os_sync_wait_on_address_with_timeout( &cell->word, (uint64_t)(uint32_t)state, 4,
                                                        OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                        OS_CLOCK_MACH_ABSOLUTE_TIME, timeout_ns );
        return ret < 0 ? -1 : 0;
    }
#endif
    now = clock_gettime_nsec_np( CLOCK_UPTIME_RAW );
    deadline = now + timeout_ns;
    for (;;)
    {
        struct timespec ts;
        uint64_t step;

        if (ios_cell_state( ios_cell_load( cell )) != state) return 0;
        if (now >= deadline)
        {
            errno = ETIMEDOUT;
            return -1;
        }
        step = deadline - now < IOS_PARK_POLL_NS ? deadline - now : IOS_PARK_POLL_NS;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)step;
        nanosleep( &ts, NULL );
        now = clock_gettime_nsec_np( CLOCK_UPTIME_RAW );
    }
}

void ios_cell_wake( struct ios_sync_cell *cell, int all )
{
#ifdef MAC_OS_VERSION_14_4
    void *addr = &cell->word;

    if (__builtin_available( macOS 14.4, iOS 17.4, * ))
    {
        if (all) os_sync_wake_by_address_all( addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE );
        else os_sync_wake_by_address_any( addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE );
        return;
    }
#endif
    /* below iOS 17.4 parked threads poll (ios_cell_park) */
    (void)cell;
    (void)all;
}

#elif defined(__linux__)

/* host tests: the futex system call stands in for the address wait (spec 16 Part J) */
int ios_cell_park( struct ios_sync_cell *cell, int32_t state, uint64_t timeout_ns )
{
    struct timespec ts;

    if (!timeout_ns)
    {
        errno = ETIMEDOUT;
        return -1;
    }
    ts.tv_sec  = timeout_ns / 1000000000u;
    ts.tv_nsec = timeout_ns % 1000000000u;
    if (syscall( SYS_futex, (uint32_t *)&cell->word, FUTEX_WAIT_PRIVATE, (uint32_t)state, &ts, NULL, 0 ) < 0)
    {
        if (errno == EAGAIN || errno == EINTR) return 0;
        return -1;
    }
    return 0;
}

void ios_cell_wake( struct ios_sync_cell *cell, int all )
{
    syscall( SYS_futex, (uint32_t *)&cell->word, FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, NULL, NULL, 0 );
}

#else
#error "ios_sync_cells.c: no address wait for this platform"
#endif

/* ---- C1: allocation, on the server thread only (no lock) ---- */

static int free_head = -1;                  /* last freed first */
static int32_t free_next[IOS_SYNC_CELLS];
static int bump;                            /* cells below this were allocated once */
static int full_logged;
static int in_use;

static uint32_t next_gen( uint32_t gen )
{
    return ++gen ? gen : 1;   /* 0 means "never allocated" */
}

int ios_cell_alloc( uint32_t kind, uint32_t manual, uint32_t max, int32_t state )
{
    struct ios_sync_cell *cell;
    int index;

    if (free_head >= 0)
    {
        index = free_head;
        free_head = free_next[index];
    }
    else if (bump < IOS_SYNC_CELLS) index = bump++;
    else
    {
        if (!full_logged)
        {
            full_logged = 1;
            ios_sync_log( "fastsync: the cell table is full (%d cells); new objects use the server",
                          IOS_SYNC_CELLS );
        }
        return -1;
    }

    cell = ios_cell_at( index );
    /* immutable for the generation, written before the store that publishes it */
    __atomic_store_n( &cell->manual, manual, __ATOMIC_RELAXED );
    __atomic_store_n( &cell->kind, kind, __ATOMIC_RELAXED );
    __atomic_store_n( &cell->max, max, __ATOMIC_RELAXED );
    __atomic_store_n( &cell->srv_waiters, 0, __ATOMIC_SEQ_CST );
    __atomic_store_n( &cell->cli_waiters, 0, __ATOMIC_SEQ_CST );
    ios_cell_store( cell, ios_cell_make( next_gen( ios_cell_gen( ios_cell_load( cell ))), state ));
    __atomic_store_n( &in_use, 1, __ATOMIC_RELAXED );
    return index;
}

void ios_cell_free( int index )
{
    struct ios_sync_cell *cell;

    if (index < 0 || index >= IOS_SYNC_CELLS) return;
    cell = ios_cell_at( index );
    /* a parked client is released by the value change */
    ios_cell_store( cell, ios_cell_make( next_gen( ios_cell_gen( ios_cell_load( cell ))), IOS_CELL_DISABLED ));
    if (ios_cell_cli_waiters( cell )) ios_cell_wake( cell, 1 );
    free_next[index] = free_head;
    free_head = index;
}

int ios_cells_in_use(void)
{
    return __atomic_load_n( &in_use, __ATOMIC_RELAXED );
}
