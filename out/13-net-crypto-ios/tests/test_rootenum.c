// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part K: root certificate enumeration (Part F2, common/ios_rootenum.c) with fake
// threads, fake times and five fake certificates of different sizes.

#include <stdlib.h>
#include <string.h>

#include "ios_rootenum.h"
#include "ios_test.h"

#define NCERTS 5
static const unsigned char c0[] = "cert-zero";
static const unsigned char c1[] = "cert-one!!";
static const unsigned char c2[] = "c2";
static const unsigned char c3[] = "certificate-three";
static const unsigned char c4[] = "4";
static const unsigned char *const data[NCERTS] = { c0, c1, c2, c3, c4 };
static const size_t sizes[NCERTS] = { sizeof(c0), sizeof(c1), sizeof(c2), sizeof(c3), sizeof(c4) };

static struct ios_rootenum e;

/* one call with a big enough buffer; returns the index of the certificate copied, -1 at the
 * end, -2 on anything unexpected */
static int next( uint64_t thread, uint64_t now )
{
    unsigned char buf[64];
    size_t needed = 0;
    int i;

    memset( buf, 0xcc, sizeof(buf) );
    if (ios_rootenum_next( &e, thread, now, NCERTS, sizes, data, buf, sizeof(buf), &needed ) == IOS_ROOTENUM_END)
        return -1;
    for (i = 0; i < NCERTS; i++)
        if (needed == sizes[i] && !memcmp( buf, data[i], sizes[i] )) return i;
    return -2;
}

/* a whole pass: every certificate once, in order, then the end */
static int full_pass( uint64_t thread, uint64_t now )
{
    int i;

    for (i = 0; i < NCERTS; i++)
        if (next( thread, now ) != i) return 0;
    return next( thread, now ) == -1;
}

int main( void )
{
    unsigned char small[4];
    size_t needed;
    int i, a, b;

    /* one thread sees every certificate once per pass */
    memset( &e, 0, sizeof(e) );
    CHECK( full_pass( 1, 1000 ) );
    /* the end forgot the position: a second pass after the first ends sees all again */
    CHECK_EQ( ios_rootenum_slot_of( &e, 1 ), (unsigned long long)-1 );
    CHECK( full_pass( 1, 1001 ) );
    CHECK( full_pass( 1, 1002 ) );

    /* two threads interleaved each see all */
    memset( &e, 0, sizeof(e) );
    for (i = 0; i < NCERTS; i++)
    {
        a = next( 10, 2000 + i );
        b = next( 11, 2000 + i );
        CHECK_EQ( a, i );
        CHECK_EQ( b, i );
    }
    CHECK_EQ( next( 10, 2100 ), (unsigned long long)-1 );
    CHECK_EQ( next( 11, 2100 ), (unsigned long long)-1 );

    /* a too-small buffer repeats the same certificate, with its size in needed */
    memset( &e, 0, sizeof(e) );
    CHECK_EQ( next( 20, 3000 ), 0 );
    for (i = 0; i < 3; i++)
    {
        needed = 0;
        CHECK_EQ( ios_rootenum_next( &e, 20, 3001, NCERTS, sizes, data, small, sizeof(small), &needed ),
                  IOS_ROOTENUM_OK );
        CHECK_EQ( needed, sizes[1] );
    }
    /* also with no buffer at all */
    CHECK_EQ( ios_rootenum_next( &e, 20, 3001, NCERTS, sizes, data, NULL, 0, &needed ), IOS_ROOTENUM_OK );
    CHECK_EQ( needed, sizes[1] );
    CHECK_EQ( next( 20, 3002 ), 1 );   /* grown buffer: the same certificate, then on */
    CHECK_EQ( next( 20, 3002 ), 2 );
    /* c3 doesn't fit in 4 bytes: needed says its size and the position stays at 3 */
    CHECK_EQ( ios_rootenum_next( &e, 20, 3003, NCERTS, sizes, data, small, sizeof(small), &needed ),
              IOS_ROOTENUM_OK );
    CHECK_EQ( needed, sizes[3] );
    CHECK_EQ( next( 20, 3004 ), 3 );
    CHECK_EQ( next( 20, 3004 ), 4 );
    /* the end resets: the next call starts a new pass */
    CHECK_EQ( next( 20, 3005 ), (unsigned long long)-1 );
    CHECK_EQ( next( 20, 3006 ), 0 );

    /* 10 s idle restarts a pass abandoned halfway */
    memset( &e, 0, sizeof(e) );
    CHECK_EQ( next( 30, 5000 ), 0 );
    CHECK_EQ( next( 30, 5000 ), 1 );
    CHECK_EQ( next( 30, 5000 + IOS_ROOTENUM_IDLE_MS - 1 ), 2 );     /* 9.999 s: continues */
    CHECK_EQ( next( 30, 5000 + 2 * IOS_ROOTENUM_IDLE_MS + 10 ), 0 );  /* 10 s after the last: over */
    CHECK( full_pass( 31, 90000 ) );

    /* 33 threads: the 33rd takes the position used longest ago */
    memset( &e, 0, sizeof(e) );
    for (i = 0; i < IOS_ROOTENUM_SLOTS; i++)
    {
        CHECK_EQ( next( 100 + i, 10000 + i ), 0 );   /* thread 100 is the oldest */
        CHECK( ios_rootenum_slot_of( &e, 100 + i ) >= 0 );
    }
    CHECK_EQ( next( 101, 10100 ), 1 );                /* 101 is used again: no longer oldest */
    CHECK_EQ( next( 200, 10200 ), 0 );                /* the 33rd thread */
    CHECK( ios_rootenum_slot_of( &e, 200 ) >= 0 );
    CHECK_EQ( ios_rootenum_slot_of( &e, 100 ), (unsigned long long)-1 );   /* evicted */
    CHECK( ios_rootenum_slot_of( &e, 101 ) >= 0 );
    CHECK_EQ( next( 101, 10201 ), 2 );                /* 101 kept its position */
    CHECK_EQ( next( 100, 10202 ), 0 );                /* 100 comes back: starts over */

    /* an empty list ends at once, every time */
    memset( &e, 0, sizeof(e) );
    CHECK_EQ( ios_rootenum_next( &e, 1, 1, 0, sizes, data, small, sizeof(small), &needed ), IOS_ROOTENUM_END );
    CHECK_EQ( ios_rootenum_next( &e, 1, 2, 0, sizes, data, small, sizeof(small), &needed ), IOS_ROOTENUM_END );

    return ios_test_done( "test_rootenum" );
}
