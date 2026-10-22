// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part E (Part D): the thread registry of ios_sig_pure.c: register, remove, slot reuse,
// exact lookups by port and by thread id (no default entry), the count of slots in use or ever
// used, slot reads, a full table, the TEB range test, and lock-free readers while writers
// register and remove.

#include <pthread.h>
#include <string.h>

#include "ios_sig_pure.h"
#include "test_sig_check.h"

#define TEB(n) (0x7ff000000000ull + (uint64_t)(n) * 0x2000)

static void test_register_lookup(void)
{
    ios_threadreg_reset();
    CHECK_EQ( ios_threadreg_count(), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0x1103 ), 0 );

    CHECK_EQ( ios_threadreg_add( 0x1103, 1001, TEB(1) ), 0 );
    CHECK_EQ( ios_threadreg_add( 0x1207, 1002, TEB(2) ), 0 );
    CHECK_EQ( ios_threadreg_add( 0x130b, 1003, TEB(3) ), 0 );
    CHECK_EQ( ios_threadreg_count(), 3 );

    CHECK_EQ( ios_threadreg_teb_of( 0x1103 ), TEB(1) );
    CHECK_EQ( ios_threadreg_teb_of( 0x1207 ), TEB(2) );
    CHECK_EQ( ios_threadreg_teb_of( 0x130b ), TEB(3) );
    CHECK_EQ( ios_threadreg_teb_of_id( 1001 ), TEB(1) );
    CHECK_EQ( ios_threadreg_teb_of_id( 1003 ), TEB(3) );
    CHECK_EQ( ios_threadreg_mach( 0 ), 0x1103 );
    CHECK_EQ( ios_threadreg_teb( 1 ), TEB(2) );
    CHECK_EQ( ios_threadreg_mach( 2 ), 0x130b );

    /* exact matches only: no neighbour, no default entry */
    CHECK_EQ( ios_threadreg_teb_of( 0x1104 ), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0x1102 ), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0 ), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0xffffffff ), 0 );
    CHECK_EQ( ios_threadreg_teb_of_id( 1004 ), 0 );
    CHECK_EQ( ios_threadreg_teb_of_id( 0 ), 0 );

    /* slots out of range or never used read as empty */
    CHECK_EQ( ios_threadreg_teb( 3 ), 0 );
    CHECK_EQ( ios_threadreg_mach( 3 ), 0 );
    CHECK_EQ( ios_threadreg_teb( -1 ), 0 );
    CHECK_EQ( ios_threadreg_mach( IOS_THREADREG_SLOTS ), 0 );

    /* ports 0 and the internal busy marker are never registered */
    CHECK_EQ( ios_threadreg_add( 0, 1005, TEB(5) ), (uint64_t)-1 );
    CHECK_EQ( ios_threadreg_add( 0xffffffff, 1005, TEB(5) ), (uint64_t)-1 );
    CHECK_EQ( ios_threadreg_count(), 3 );
    CHECK_EQ( ios_threadreg_teb_of_id( 1005 ), 0 );

    /* id 0 (unknown) is never found, though the port is */
    CHECK_EQ( ios_threadreg_add( 0x1403, 0, TEB(4) ), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0x1403 ), TEB(4) );
    CHECK_EQ( ios_threadreg_teb_of_id( 0 ), 0 );
}

static void test_remove_reuse(void)
{
    ios_threadreg_reset();
    ios_threadreg_add( 0x2103, 2001, TEB(1) );
    ios_threadreg_add( 0x2207, 2002, TEB(2) );
    ios_threadreg_add( 0x230b, 2003, TEB(3) );

    /* removal empties the slot; the count still covers it */
    ios_threadreg_remove( 0x2207 );
    CHECK_EQ( ios_threadreg_teb_of( 0x2207 ), 0 );
    CHECK_EQ( ios_threadreg_teb_of_id( 2002 ), 0 );
    CHECK_EQ( ios_threadreg_mach( 1 ), 0 );
    CHECK_EQ( ios_threadreg_teb( 1 ), 0 );
    CHECK_EQ( ios_threadreg_count(), 3 );
    CHECK_EQ( ios_threadreg_teb_of( 0x2103 ), TEB(1) );
    CHECK_EQ( ios_threadreg_teb_of( 0x230b ), TEB(3) );

    /* removing it again, or a port never registered, changes nothing */
    ios_threadreg_remove( 0x2207 );
    ios_threadreg_remove( 0x9999 );
    ios_threadreg_remove( 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0x2103 ), TEB(1) );
    CHECK_EQ( ios_threadreg_count(), 3 );

    /* the empty slot is reused */
    CHECK_EQ( ios_threadreg_add( 0x240f, 2004, TEB(4) ), 0 );
    CHECK_EQ( ios_threadreg_mach( 1 ), 0x240f );
    CHECK_EQ( ios_threadreg_teb( 1 ), TEB(4) );
    CHECK_EQ( ios_threadreg_count(), 3 );

    /* a port registered again gets the new values in its own slot */
    CHECK_EQ( ios_threadreg_add( 0x2103, 2005, TEB(5) ), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0x2103 ), TEB(5) );
    CHECK_EQ( ios_threadreg_teb_of_id( 2005 ), TEB(5) );
    CHECK_EQ( ios_threadreg_teb_of_id( 2001 ), 0 );
    CHECK_EQ( ios_threadreg_mach( 0 ), 0x2103 );
    CHECK_EQ( ios_threadreg_count(), 3 );

    /* a new port after the last one used extends the count */
    CHECK_EQ( ios_threadreg_add( 0x2513, 2006, TEB(6) ), 0 );
    CHECK_EQ( ios_threadreg_mach( 3 ), 0x2513 );
    CHECK_EQ( ios_threadreg_count(), 4 );
}

static void test_full(void)
{
    int i;

    ios_threadreg_reset();
    for (i = 0; i < IOS_THREADREG_SLOTS; i++)
        CHECK_EQ( ios_threadreg_add( 0x10000 + i, 50000 + i, TEB(i) ), 0 );
    CHECK_EQ( ios_threadreg_count(), IOS_THREADREG_SLOTS );
    CHECK_EQ( ios_threadreg_add( 0x30000, 1, TEB(9999) ), (uint64_t)-1 );
    CHECK_EQ( ios_threadreg_teb_of( 0x30000 ), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0x10000 + IOS_THREADREG_SLOTS - 1 ), TEB(IOS_THREADREG_SLOTS - 1) );
    CHECK_EQ( ios_threadreg_teb_of_id( 50000 + 700 ), TEB(700) );

    /* full, but a port already there can still be updated */
    CHECK_EQ( ios_threadreg_add( 0x10000 + 5, 7, TEB(7777) ), 0 );
    CHECK_EQ( ios_threadreg_teb( 5 ), TEB(7777) );

    /* a freed slot in the middle takes the next thread */
    ios_threadreg_remove( 0x10000 + 600 );
    CHECK_EQ( ios_threadreg_add( 0x30000, 1, TEB(9999) ), 0 );
    CHECK_EQ( ios_threadreg_mach( 600 ), 0x30000 );
    CHECK_EQ( ios_threadreg_teb_of( 0x30000 ), TEB(9999) );
    CHECK_EQ( ios_threadreg_count(), IOS_THREADREG_SLOTS );
    ios_threadreg_reset();
    CHECK_EQ( ios_threadreg_count(), 0 );
    CHECK_EQ( ios_threadreg_teb_of( 0x30000 ), 0 );
}

static void test_range_busy(void)
{
    ios_threadreg_reset();
    CHECK( !ios_threadreg_range_busy( 0, ~(uintptr_t)0 ) );

    ios_threadreg_add( 0x3103, 3001, 0x10000 );
    CHECK( ios_threadreg_range_busy( 0x10000, 1 ) );
    CHECK( ios_threadreg_range_busy( 0xf000, 0x1001 ) );
    CHECK( !ios_threadreg_range_busy( 0xf000, 0x1000 ) );   /* the end is exclusive */
    CHECK( !ios_threadreg_range_busy( 0x10001, 0x10000 ) );
    CHECK( !ios_threadreg_range_busy( 0x10000, 0 ) );
    CHECK( ios_threadreg_range_busy( 0, ~(uintptr_t)0 ) );

    /* a removed thread's TEB doesn't count */
    ios_threadreg_remove( 0x3103 );
    CHECK( !ios_threadreg_range_busy( 0x10000, 1 ) );
}

/* ---------- lock-free readers against writers ---------- */

#define WRITERS 4
#define PORTS_PER_WRITER 16
#define ROUNDS 3000

static _Atomic int stop_readers;
static _Atomic int bad_reads;

/* port p always carries TEB 0x1000 * p and id p + 1, so any mix-up is visible */
static void *writer( void *arg )
{
    uint32_t first = 0x100 + (uint32_t)(uintptr_t)arg * PORTS_PER_WRITER;
    int round, k;

    for (round = 0; round < ROUNDS; round++)
        for (k = 0; k < PORTS_PER_WRITER; k++)
        {
            uint32_t p = first + (uint32_t)k;
            if ((round + k) & 1) ios_threadreg_remove( p );
            else ios_threadreg_add( p, p + 1, 0x1000 * (uintptr_t)p );
        }
    return NULL;
}

static void *reader( void *arg )
{
    (void)arg;
    while (!atomic_load( &stop_readers ))
    {
        int i, n = ios_threadreg_count();
        uint32_t p;

        for (p = 0x100; p < 0x100 + WRITERS * PORTS_PER_WRITER; p++)
        {
            uintptr_t t = ios_threadreg_teb_of( p ), u = ios_threadreg_teb_of_id( p + 1 );
            if (t && t != 0x1000 * (uintptr_t)p) atomic_fetch_add( &bad_reads, 1 );
            if (u && u != 0x1000 * (uintptr_t)p) atomic_fetch_add( &bad_reads, 1 );
        }
        for (i = 0; i < n; i++)
        {
            /* a slot's two values come from different reads, so only the TEB is checked */
            uintptr_t t = ios_threadreg_teb( i );
            if (t && (t % 0x1000 || t / 0x1000 < 0x100 || t / 0x1000 >= 0x100 + WRITERS * PORTS_PER_WRITER))
                atomic_fetch_add( &bad_reads, 1 );
        }
    }
    return NULL;
}

static void test_concurrent(void)
{
    pthread_t w[WRITERS], r[2];
    int i;

    ios_threadreg_reset();
    atomic_store( &stop_readers, 0 );
    atomic_store( &bad_reads, 0 );
    for (i = 0; i < 2; i++) pthread_create( &r[i], NULL, reader, NULL );
    for (i = 0; i < WRITERS; i++) pthread_create( &w[i], NULL, writer, (void *)(uintptr_t)i );
    for (i = 0; i < WRITERS; i++) pthread_join( w[i], NULL );
    atomic_store( &stop_readers, 1 );
    for (i = 0; i < 2; i++) pthread_join( r[i], NULL );

    CHECK_EQ( atomic_load( &bad_reads ), 0 );
    /* never more slots than ports, and every port is where its last operation left it */
    CHECK( ios_threadreg_count() <= WRITERS * PORTS_PER_WRITER );
    for (i = 0; i < WRITERS * PORTS_PER_WRITER; i++)
    {
        uint32_t p = 0x100 + (uint32_t)i;
        int k = i % PORTS_PER_WRITER;
        uintptr_t want = ((ROUNDS - 1 + k) & 1) ? 0 : 0x1000 * (uintptr_t)p;
        CHECK_EQ( ios_threadreg_teb_of( p ), want );
    }
}

int main(void)
{
    test_register_lookup();
    test_remove_reuse();
    test_full();
    test_range_busy();
    test_concurrent();
    return check_summary();
}
