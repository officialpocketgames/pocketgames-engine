// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Part J, Part H: sock.c's own tcp_state_to_mib_state (extracted from server/sock.c by
// run-host-tests.sh into tcp_map.inc) with Darwin's BSD state numbers (server/ios_s17_pure.h):
// every BSD number maps to its MIB_TCP_STATE. Also the virtual desktop's clip size (H3).

#include <stdint.h>
#include <stdio.h>

#include <windef.h>
#include <in6addr.h>
#include <tcpmib.h>

#define IOS_S17_TCP_STATES
#include "ios_s17_pure.h"
#include "tcp_map.inc"

static int failures, checks;

#define CHECK_EQ(a, b) do { long a_ = (long)(a), b_ = (long)(b); checks++; \
    if (a_ != b_) { failures++; printf( "FAIL %s:%d: %s == %s (%ld != %ld)\n", \
        __FILE__, __LINE__, #a, #b, a_, b_ ); } } while (0)

static void test_tcp_states(void)
{
    /* the numbers of the macOS SDK's netinet/tcp_fsm.h */
    CHECK_EQ( tcp_state_to_mib_state( 0 ), MIB_TCP_STATE_CLOSED );
    CHECK_EQ( tcp_state_to_mib_state( 1 ), MIB_TCP_STATE_LISTEN );
    CHECK_EQ( tcp_state_to_mib_state( 2 ), MIB_TCP_STATE_SYN_SENT );
    CHECK_EQ( tcp_state_to_mib_state( 3 ), MIB_TCP_STATE_SYN_RCVD );
    CHECK_EQ( tcp_state_to_mib_state( 4 ), MIB_TCP_STATE_ESTAB );
    CHECK_EQ( tcp_state_to_mib_state( 5 ), MIB_TCP_STATE_CLOSE_WAIT );
    CHECK_EQ( tcp_state_to_mib_state( 6 ), MIB_TCP_STATE_FIN_WAIT1 );
    CHECK_EQ( tcp_state_to_mib_state( 7 ), MIB_TCP_STATE_CLOSING );
    CHECK_EQ( tcp_state_to_mib_state( 8 ), MIB_TCP_STATE_LAST_ACK );
    CHECK_EQ( tcp_state_to_mib_state( 9 ), MIB_TCP_STATE_FIN_WAIT2 );
    CHECK_EQ( tcp_state_to_mib_state( 10 ), MIB_TCP_STATE_TIME_WAIT );
    /* anything else is closed, as upstream */
    CHECK_EQ( tcp_state_to_mib_state( 11 ), MIB_TCP_STATE_CLOSED );
    CHECK_EQ( tcp_state_to_mib_state( -1 ), MIB_TCP_STATE_CLOSED );
}

static void test_virtual_desktop_size(void)
{
    int w = 1280, h = 720;

    ios_s17_virtual_desktop_size( 1, &w, &h );
    CHECK_EQ( w, 1280 );
    CHECK_EQ( h, 720 );
    w = 1280; h = 720;
    ios_s17_virtual_desktop_size( 0, &w, &h );    /* nothing known */
    CHECK_EQ( w, 1024 );
    CHECK_EQ( h, 768 );
    w = 0; h = 720;
    ios_s17_virtual_desktop_size( 1, &w, &h );    /* not positive */
    CHECK_EQ( w, 1024 );
    CHECK_EQ( h, 768 );
    w = 800; h = -1;
    ios_s17_virtual_desktop_size( 1, &w, &h );
    CHECK_EQ( w, 1024 );
    CHECK_EQ( h, 768 );
}

int main(void)
{
    test_tcp_states();
    test_virtual_desktop_size();
    printf( "%d checks, %d failed\n", checks, failures );
    return failures ? 1 : 0;
}
