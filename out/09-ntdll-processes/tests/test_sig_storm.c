// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part E (Part B4): the signal-storm counter of ios_sig_pure.c. signal_arm64_ios.c's
// check_storm ends the pseudo-process when ios_storm_note returns IOS_STORM_LIMIT or more, so the
// 1000th identical fault in a row trips it and the 999th doesn't.

#include <string.h>

#include "ios_sig_pure.h"
#include "test_sig_check.h"

#define SIGSEGV_ 11
#define SIGBUS_  10

static int trips( unsigned int count ) { return count >= IOS_STORM_LIMIT; }  /* as check_storm */

static void test_counting(void)
{
    struct ios_storm s;
    unsigned int i, n = 0;

    memset( &s, 0, sizeof(s) );
    /* a zeroed counter: the first fault is new, even with all-zero values */
    CHECK_EQ( ios_storm_note( &s, 0, 0, 0 ), 1 );
    CHECK_EQ( ios_storm_note( &s, 0, 0, 0 ), 2 );

    ios_storm_reset( &s );
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1000, 0x20 ), 1 );
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1000, 0x20 ), 2 );
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1000, 0x20 ), 3 );

    /* a different pc, address or signal starts again */
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1004, 0x20 ), 1 );
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1004, 0x20 ), 2 );
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1004, 0x28 ), 1 );
    CHECK_EQ( ios_storm_note( &s, SIGBUS_, 0x1004, 0x28 ), 1 );
    CHECK_EQ( ios_storm_note( &s, SIGBUS_, 0x1004, 0x28 ), 2 );
    /* going back to an earlier fault doesn't resume its count */
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1000, 0x20 ), 1 );

    /* progress (an emulated access) resets it */
    ios_storm_reset( &s );
    CHECK_EQ( ios_storm_note( &s, SIGSEGV_, 0x1000, 0x20 ), 1 );

    /* alternating faults never build up */
    ios_storm_reset( &s );
    for (i = 0; i < 3 * IOS_STORM_LIMIT; i++)
        n = ios_storm_note( &s, SIGSEGV_, (i & 1) ? 0x2000 : 0x2004, 0x30 );
    CHECK_EQ( n, 1 );
}

static void test_limit(void)
{
    struct ios_storm s;
    unsigned int i, n = 0, tripped_at = 0;

    CHECK_EQ( IOS_STORM_LIMIT, 1000 );

    memset( &s, 0, sizeof(s) );
    for (i = 1; i <= IOS_STORM_LIMIT + 5; i++)
    {
        n = ios_storm_note( &s, SIGSEGV_, 0x140001000ull, 0 );
        CHECK_EQ( n, i );
        if (!tripped_at && trips( n )) tripped_at = i;
    }
    CHECK_EQ( tripped_at, 1000 );

    /* 999 in a row, then one other fault: no trip, and the count starts over */
    memset( &s, 0, sizeof(s) );
    for (i = 0; i < IOS_STORM_LIMIT - 1; i++) n = ios_storm_note( &s, SIGBUS_, 0x5000, 0x6000 );
    CHECK_EQ( n, 999 );
    CHECK( !trips( n ) );
    n = ios_storm_note( &s, SIGBUS_, 0x5000, 0x6008 );
    CHECK_EQ( n, 1 );
    for (i = 0; i < IOS_STORM_LIMIT - 1; i++) n = ios_storm_note( &s, SIGBUS_, 0x5000, 0x6008 );
    CHECK_EQ( n, 1000 );
    CHECK( trips( n ) );

    /* check_storm resets after a trip: the next fault is new */
    ios_storm_reset( &s );
    CHECK_EQ( ios_storm_note( &s, SIGBUS_, 0x5000, 0x6008 ), 1 );
}

int main(void)
{
    test_counting();
    test_limit();
    return check_summary();
}
