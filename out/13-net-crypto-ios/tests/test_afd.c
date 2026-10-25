// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part K: the AFD rules of Part G3 (ntdll-unix/ios_afd.h): the GET_EVENTS event-handle
// correction below, at, inside and beyond B + 4 GB, with B = 0 and with the switch off; the
// guest addresses of AFD requests.

#define IOS_AFD_PURE_ONLY
#include "ios_afd.h"
#include "ios_test.h"

int main( void )
{
    const uint64_t B = 0x500000000ull;
    const uint64_t FOUR_GB = 0x100000000ull;

    /* the event handle: inside [B, B + 4 GB) it is (LONG)(v - B), sign-extended */
    CHECK_EQ( ios_afd_event_handle_at( B, B + 0x1a4, 1 ), 0x1a4 );        /* a real handle */
    CHECK_EQ( ios_afd_event_handle_at( B, B, 1 ), 0 );                    /* at B: NULL */
    CHECK_EQ( ios_afd_event_handle_at( B, B + 0x7fffffff, 1 ), 0x7fffffff );
    CHECK_EQ( ios_afd_event_handle_at( B, B + 0x80000000ull, 1 ), 0xffffffff80000000ull );
    CHECK_EQ( ios_afd_event_handle_at( B, B + 0xfffffffeull, 1 ), (uint64_t)-2 );   /* sign-extended */
    CHECK_EQ( ios_afd_event_handle_at( B, B + 0xffffffffull, 1 ), (uint64_t)-1 );   /* last inside */
    /* beyond B + 4 GB and below B: passed unchanged */
    CHECK_EQ( ios_afd_event_handle_at( B, B + FOUR_GB, 1 ), B + FOUR_GB );
    CHECK_EQ( ios_afd_event_handle_at( B, B + FOUR_GB + 0x10, 1 ), B + FOUR_GB + 0x10 );
    CHECK_EQ( ios_afd_event_handle_at( B, B - 1, 1 ), B - 1 );
    CHECK_EQ( ios_afd_event_handle_at( B, 0x1a4, 1 ), 0x1a4 );            /* not converted at all */
    CHECK_EQ( ios_afd_event_handle_at( B, 0, 1 ), 0 );                    /* NULL stays NULL */
    /* B = 0: a 64-bit caller, unchanged */
    CHECK_EQ( ios_afd_event_handle_at( 0, 0x1a4, 1 ), 0x1a4 );
    CHECK_EQ( ios_afd_event_handle_at( 0, 0x7f0000001000ull, 1 ), 0x7f0000001000ull );
    CHECK_EQ( ios_afd_event_handle_at( 0, 0, 1 ), 0 );
    /* the switch off: unchanged */
    CHECK_EQ( ios_afd_event_handle_at( B, B + 0x1a4, 0 ), B + 0x1a4 );
    CHECK_EQ( ios_afd_event_handle_at( B, B, 0 ), B );

    /* PG_AFD_EVENT_HANDLE: only "0" turns it off */
    CHECK( ios_afd_event_env_enabled( NULL ) );
    CHECK( ios_afd_event_env_enabled( "" ) );
    CHECK( ios_afd_event_env_enabled( "1" ) );
    CHECK( !ios_afd_event_env_enabled( "0" ) );
    CHECK( ios_afd_event_env_enabled( "00" ) );

    /* guest addresses of AFD requests: B + (low 32 bits), 0 stays NULL; as is for B = 0 */
    CHECK_EQ( (uintptr_t)ios_afd_user_ptr_at( B, 0x00401000 ), B + 0x00401000 );
    CHECK_EQ( (uintptr_t)ios_afd_user_ptr_at( B, 0xfffff000u ), B + 0xfffff000u );
    CHECK( ios_afd_user_ptr_at( B, 0 ) == NULL );
    CHECK_EQ( (uintptr_t)ios_afd_user_ptr_at( B, 0xdead00000000ull | 0x2000 ), B + 0x2000 );  /* low 32 bits */
    CHECK_EQ( (uintptr_t)ios_afd_user_ptr_at( 0, 0x7f0000001000ull ), 0x7f0000001000ull );
    CHECK( ios_afd_user_ptr_at( 0, 0 ) == NULL );

    return ios_test_done( "test_afd" );
}
