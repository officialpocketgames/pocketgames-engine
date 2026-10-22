// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part K: dnsapi's resolver layer (Part H2 with clarification 3, common/ios_resolv.c)
// with fakes for res_ninit / res_nquery / res_ndestroy: one state per thread, init on that
// state, a failed query's res_h_errno copied to our own h_errno, the state released with
// res_ndestroy at thread exit only when it was initialised.

#include <pthread.h>

#ifndef IOS_RESOLV_TEST
#define IOS_RESOLV_TEST
#endif
#define IOS_RESOLV_NO_MACROS
#include "ios_resolv.h"
#include "ios_test.h"

/* ---- fakes ---- */
static int ninit_calls, ninit_result;
static res_state last_ninit;
static int nquery_result, nquery_herrno;
static int ndestroy_calls;
static res_state last_ndestroy;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

int ios_test_res_ninit( res_state state )
{
    pthread_mutex_lock( &fake_lock );
    ninit_calls++;
    last_ninit = state;
    pthread_mutex_unlock( &fake_lock );
    if (ninit_result) return ninit_result;
    state->options |= RES_INIT;
    state->nscount = 1;
    return 0;
}

int ios_test_res_nquery( res_state state, const char *dname, int klass, int type,
                         unsigned char *answer, int anslen )
{
    if (nquery_result < 0) state->res_h_errno = nquery_herrno;
    return nquery_result;
}

void ios_test_res_ndestroy( res_state state )
{
    pthread_mutex_lock( &fake_lock );
    ndestroy_calls++;
    last_ndestroy = state;
    pthread_mutex_unlock( &fake_lock );
}

/* ---- threads ---- */
static res_state other_state;
static int other_nscount;

static void *thread_init( void *arg )
{
    other_state = ios_resolv_state();
    other_nscount = other_state->nscount;
    ios_resolv_init();
    return NULL;
}

static void *thread_no_init( void *arg )
{
    other_state = ios_resolv_state();
    return NULL;
}

int main( void )
{
    unsigned char answer[512];
    res_state mine;
    pthread_t thread;

    /* one state per thread, the same on every call, zeroed before init */
    mine = ios_resolv_state();
    CHECK( mine != NULL );
    CHECK( ios_resolv_state() == mine );
    CHECK_EQ( mine->options & RES_INIT, 0 );
    CHECK_EQ( mine->nscount, 0 );

    /* init is res_ninit on this thread's state */
    CHECK_EQ( ios_resolv_init(), 0 );
    CHECK_EQ( ninit_calls, 1 );
    CHECK( last_ninit == mine );
    CHECK( mine->options & RES_INIT );

    /* a failed query: the state's res_h_errno becomes our h_errno */
    *ios_resolv_h_errno_location() = 0;
    nquery_result = -1;
    nquery_herrno = TRY_AGAIN;              /* -> DNS_ERROR_RCODE_SERVER_FAILURE upstream */
    CHECK_EQ( ios_resolv_query( "example.com", 1, 1, answer, sizeof(answer) ), (unsigned long long)-1 );
    CHECK_EQ( *ios_resolv_h_errno_location(), TRY_AGAIN );
    nquery_herrno = HOST_NOT_FOUND;
    CHECK_EQ( ios_resolv_query( "nowhere.invalid", 1, 1, answer, sizeof(answer) ), (unsigned long long)-1 );
    CHECK_EQ( *ios_resolv_h_errno_location(), HOST_NOT_FOUND );
    /* a successful one returns the length and leaves h_errno alone */
    nquery_result = 42;
    CHECK_EQ( ios_resolv_query( "example.com", 1, 1, answer, sizeof(answer) ), 42 );
    CHECK_EQ( *ios_resolv_h_errno_location(), HOST_NOT_FOUND );

    /* another thread has its own state, released with res_ndestroy when it exits */
    mine->nscount = 3;
    CHECK( !pthread_create( &thread, NULL, thread_init, NULL ) );
    pthread_join( thread, NULL );
    CHECK( other_state != mine );
    CHECK_EQ( other_nscount, 0 );
    CHECK_EQ( ninit_calls, 2 );
    CHECK_EQ( ndestroy_calls, 1 );
    CHECK( last_ndestroy == other_state );
    CHECK_EQ( mine->nscount, 3 );

    /* a thread that never initialised its state: freed without res_ndestroy */
    CHECK( !pthread_create( &thread, NULL, thread_no_init, NULL ) );
    pthread_join( thread, NULL );
    CHECK_EQ( ndestroy_calls, 1 );

    /* a failing res_ninit is reported, not hidden */
    ninit_result = -1;
    CHECK_EQ( ios_resolv_init(), (unsigned long long)-1 );

    return ios_test_done( "test_resolv" );
}
