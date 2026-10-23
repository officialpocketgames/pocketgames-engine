// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part H2 with clarification 3: the host resolver for dnsapi's unix side, through the
// public res_ninit / res_nquery / res_ndestroy of <resolv.h>, linked with -lresolv. See
// include/ios_resolv.h.
//
// State: one resolver state and our h_errno per thread. The state is allocated on the thread's
// first use and handed to a pthread key whose destructor releases it with res_ndestroy() when
// the thread exits (Wine threads are short-lived often enough that leaking it would add up).

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IOS_RESOLV_NO_MACROS
#include "ios_resolv.h"

#ifdef IOS_RESOLV_TEST
# define IOS_RES_NINIT     ios_test_res_ninit
# define IOS_RES_NQUERY    ios_test_res_nquery
# define IOS_RES_NDESTROY  ios_test_res_ndestroy
#else
# define IOS_RES_NINIT     res_ninit
# define IOS_RES_NQUERY    res_nquery
# ifdef __APPLE__
#  define IOS_RES_NDESTROY res_ndestroy
# else
#  define IOS_RES_NDESTROY res_nclose      /* glibc (host builds) has no res_ndestroy */
# endif
#endif

typedef __typeof__(*(res_state)0) ios_res_state_t;

static pthread_once_t ios_resolv_once = PTHREAD_ONCE_INIT;
static pthread_key_t ios_resolv_key;
static int ios_resolv_key_ok;

static __thread res_state ios_state;
/* used only when the allocation fails: a state that is never released */
static __thread ios_res_state_t ios_static_state;
static __thread int ios_h_errno;

static void ios_resolv_release( void *ptr )
{
    res_state state = ptr;

    /* only an initialised state owns anything (a zeroed one has _vcsock 0, which a close would
     * take for stdin) */
    if (state->options & RES_INIT) IOS_RES_NDESTROY( state );
    free( state );
}

static void ios_resolv_make_key( void )
{
    ios_resolv_key_ok = !pthread_key_create( &ios_resolv_key, ios_resolv_release );
}

res_state ios_resolv_state( void )
{
    res_state state;

    if (ios_state) return ios_state;

    pthread_once( &ios_resolv_once, ios_resolv_make_key );
    if (ios_resolv_key_ok && (state = calloc( 1, sizeof(*state) )))
    {
        if (!pthread_setspecific( ios_resolv_key, state )) return ios_state = state;
        free( state );
    }
    return ios_state = &ios_static_state;
}

int ios_resolv_init( void )
{
    static int logged;
    int ret = IOS_RES_NINIT( ios_resolv_state() );

    if (ret && !__atomic_exchange_n( &logged, 1, __ATOMIC_RELAXED ))
        fprintf( stderr, "[dnsapi-ios] res_ninit failed (%d): no DNS servers\n", ret );
    return ret;
}

int ios_resolv_query( const char *dname, int klass, int type, unsigned char *answer, int anslen )
{
    res_state state = ios_resolv_state();
    int ret = IOS_RES_NQUERY( state, dname, klass, type, answer, anslen );

    if (ret < 0) ios_h_errno = state->res_h_errno;
    return ret;
}

int *ios_resolv_h_errno_location( void )
{
    return &ios_h_errno;
}
