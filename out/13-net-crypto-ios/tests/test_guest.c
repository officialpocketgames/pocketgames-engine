// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part K: Part B's conversions (include/ios_guest.h) with fake window bases.

#define IOS_GUEST_PURE_ONLY
#include "ios_guest.h"
#include "ios_test.h"

int main( void )
{
    const uint64_t B = 0x300000000ull;   /* a 4 GB aligned window base */
    char host_object = 0;   /* only its address is used */

    /* NULL both ways */
    CHECK( ios_guest_to_host_at( B, 0 ) == NULL );
    CHECK_EQ( ios_host_to_guest_at( B, NULL ), 0 );

    /* B + a into the host */
    CHECK_EQ( (uintptr_t)ios_guest_to_host_at( B, 0x1000 ), B + 0x1000 );
    CHECK_EQ( (uintptr_t)ios_guest_to_host_at( B, 0xfffff000u ), B + 0xfffff000u );
    CHECK_EQ( (uintptr_t)ios_guest_to_host_at( B, 0x7ffe0000u ), B + 0x7ffe0000u );

    /* host - B back to the program */
    CHECK_EQ( ios_host_to_guest_at( B, (void *)(uintptr_t)(B + 0x1234) ), 0x1234 );
    CHECK_EQ( ios_host_to_guest_at( B, (void *)(uintptr_t)(B + 0xffffffffu) ), 0xffffffffu );

    /* round trip */
    CHECK_EQ( ios_host_to_guest_at( B, ios_guest_to_host_at( B, 0x00401000 ) ), 0x00401000 );

    /* identity with B = 0: exactly upstream's ULongToPtr / PtrToUlong */
    CHECK_EQ( (uintptr_t)ios_guest_to_host_at( 0, 0x1000 ), 0x1000 );
    CHECK_EQ( (uintptr_t)ios_guest_to_host_at( 0, 0xffffffffu ), 0xffffffffu );
    CHECK( ios_guest_to_host_at( 0, 0 ) == NULL );
    CHECK_EQ( ios_host_to_guest_at( 0, (void *)(uintptr_t)0x12345678 ), 0x12345678 );
    CHECK_EQ( ios_host_to_guest_at( 0, &host_object ), (uint32_t)(uintptr_t)&host_object );

    /* dnsapi's set_serverlist block: a 32-bit NULL arriving as B, or as NULL, is NULL */
    CHECK( ios_guest_args_is_null( B, (void *)(uintptr_t)B ) );
    CHECK( ios_guest_args_is_null( B, NULL ) );
    CHECK( !ios_guest_args_is_null( B, (void *)(uintptr_t)(B + 0x2000) ) );
    /* a 64-bit caller (B = 0): only NULL is NULL */
    CHECK( ios_guest_args_is_null( 0, NULL ) );
    CHECK( !ios_guest_args_is_null( 0, &host_object ) );

    return ios_test_done( "test_guest" );
}
