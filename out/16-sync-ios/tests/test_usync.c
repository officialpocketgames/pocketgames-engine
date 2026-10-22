// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part J1: the in-process ntsync library (Part E) against the ntsync documentation.

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ios_sync.h"
#include "linux/ntsync.h"
#include "check.h"

#define DEV IOS_USYNC_DEVICE
#define INF (~(__u64)0)

static uint64_t now_ns( clockid_t clock )
{
    struct timespec ts;
    clock_gettime( clock, &ts );
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static void sleep_ms( unsigned int ms )
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000l };
    nanosleep( &ts, NULL );
}

static int mk_sem( unsigned int count, unsigned int max )
{
    struct ntsync_sem_args args = { count, max };
    return ios_usync_ioctl( DEV, NTSYNC_IOC_CREATE_SEM, &args );
}

static int mk_mutex( unsigned int owner, unsigned int count )
{
    struct ntsync_mutex_args args = { owner, count };
    return ios_usync_ioctl( DEV, NTSYNC_IOC_CREATE_MUTEX, &args );
}

static int mk_event( int manual, int signaled )
{
    struct ntsync_event_args args = { manual, signaled };
    return ios_usync_ioctl( DEV, NTSYNC_IOC_CREATE_EVENT, &args );
}

/* 0 or the errno; *index the reported index */
static int do_wait( const int *fds, unsigned int count, int all, unsigned int owner, __u64 timeout,
                    int realtime, int alert, unsigned int *index )
{
    struct ntsync_wait_args args;

    memset( &args, 0, sizeof(args) );
    args.timeout = timeout;
    args.objs = (uintptr_t)fds;
    args.count = count;
    args.owner = owner;
    args.alert = alert;
    args.index = ~0u;
    if (realtime) args.flags = NTSYNC_WAIT_REALTIME;
    if (ios_usync_ioctl( DEV, all ? NTSYNC_IOC_WAIT_ALL : NTSYNC_IOC_WAIT_ANY, &args ) < 0)
    {
        if (index) *index = args.index;
        return errno;
    }
    if (index) *index = args.index;
    return 0;
}

static int poll1( int fd, unsigned int owner )
{
    return do_wait( &fd, 1, 0, owner, 0, 0, 0, NULL );
}

static unsigned int sem_count( int fd )
{
    struct ntsync_sem_args args = { 0, 0 };
    CHECK_EQ( ios_usync_ioctl( fd, NTSYNC_IOC_SEM_READ, &args ), 0 );
    return args.count;
}

static int event_state( int fd )
{
    struct ntsync_event_args args = { 0, 0 };
    CHECK_EQ( ios_usync_ioctl( fd, NTSYNC_IOC_EVENT_READ, &args ), 0 );
    return args.signaled;
}

static void test_semaphore(void)
{
    struct ntsync_sem_args args = { 3, 2 };
    __u32 n;
    int s;

    CHECK_EQ( ios_usync_ioctl( DEV, NTSYNC_IOC_CREATE_SEM, &args ), -1 );
    CHECK_EQ( errno, EINVAL );
    s = mk_sem( 1, 3 );
    CHECK( ((unsigned int)s & 0xf0000000u) == IOS_USYNC_FD_TAG );
    CHECK( ((unsigned int)s & 0x0fffffffu) != 0 );
    n = 2;
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_SEM_RELEASE, &n ), 0 );
    CHECK_EQ( n, 1 );   /* the previous count */
    CHECK_EQ( sem_count( s ), 3 );
    n = 1;
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_SEM_RELEASE, &n ), -1 );
    CHECK_EQ( errno, EOVERFLOW );
    CHECK_EQ( sem_count( s ), 3 );
    n = 0xffffffff;
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_SEM_RELEASE, &n ), -1 );
    CHECK_EQ( errno, EOVERFLOW );
    CHECK_EQ( poll1( s, 1 ), 0 );
    CHECK_EQ( poll1( s, 1 ), 0 );
    CHECK_EQ( poll1( s, 1 ), 0 );
    CHECK_EQ( poll1( s, 1 ), ETIMEDOUT );
    CHECK_EQ( sem_count( s ), 0 );
    args.count = 7; args.max = 0;
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_SEM_READ, &args ), 0 );
    CHECK_EQ( args.max, 3 );
    /* wrong type, unknown request */
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_EVENT_SET, &n ), -1 );
    CHECK_EQ( errno, EINVAL );
    CHECK_EQ( ios_usync_ioctl( s, 0x1234, &n ), -1 );
    CHECK_EQ( errno, ENOTTY );
    CHECK_EQ( ios_usync_ioctl( DEV, 0x1234, &n ), -1 );
    CHECK_EQ( errno, ENOTTY );
    CHECK_EQ( ios_usync_close( s ), 0 );
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_SEM_READ, &args ), -1 );
    CHECK_EQ( errno, EBADF );
    CHECK_EQ( ios_usync_close( s ), -1 );
    CHECK_EQ( errno, EBADF );
    CHECK_EQ( ios_usync_close( DEV ), 0 );   /* the device: nothing happens */
}

static void test_mutex(void)
{
    struct ntsync_mutex_args args;
    unsigned int index;
    __u32 owner;
    int m, fds[2];

    args.owner = 0; args.count = 1;
    CHECK_EQ( ios_usync_ioctl( DEV, NTSYNC_IOC_CREATE_MUTEX, &args ), -1 );
    CHECK_EQ( errno, EINVAL );
    args.owner = 1; args.count = 0;
    CHECK_EQ( ios_usync_ioctl( DEV, NTSYNC_IOC_CREATE_MUTEX, &args ), -1 );
    CHECK_EQ( errno, EINVAL );

    m = mk_mutex( 0, 0 );
    CHECK_EQ( poll1( m, 5 ), 0 );
    CHECK_EQ( poll1( m, 5 ), 0 );   /* recursion */
    CHECK_EQ( poll1( m, 6 ), ETIMEDOUT );
    args.owner = 6;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_UNLOCK, &args ), -1 );
    CHECK_EQ( errno, EPERM );
    args.owner = 0;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_UNLOCK, &args ), -1 );
    CHECK_EQ( errno, EINVAL );
    args.owner = 5;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_UNLOCK, &args ), 0 );
    CHECK_EQ( args.count, 2 );   /* the previous count */
    memset( &args, 0, sizeof(args) );
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_READ, &args ), 0 );
    CHECK_EQ( args.owner, 5 );
    CHECK_EQ( args.count, 1 );
    args.owner = 5;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_UNLOCK, &args ), 0 );
    CHECK_EQ( args.count, 1 );
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_UNLOCK, &args ), -1 );
    CHECK_EQ( errno, EPERM );

    /* kill: only the owner; abandoned until someone acquires it, reported once */
    CHECK_EQ( poll1( m, 5 ), 0 );
    owner = 6;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_KILL, &owner ), -1 );
    CHECK_EQ( errno, EPERM );
    owner = 0;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_KILL, &owner ), -1 );
    CHECK_EQ( errno, EINVAL );
    owner = 5;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_KILL, &owner ), 0 );
    args.owner = args.count = 9;
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_READ, &args ), -1 );
    CHECK_EQ( errno, EOWNERDEAD );
    CHECK_EQ( args.owner, 0 );
    CHECK_EQ( args.count, 0 );
    fds[0] = mk_event( 1, 0 );
    fds[1] = m;
    CHECK_EQ( do_wait( fds, 2, 0, 7, 0, 0, 0, &index ), EOWNERDEAD );
    CHECK_EQ( index, 1 );
    CHECK_EQ( ios_usync_ioctl( m, NTSYNC_IOC_MUTEX_READ, &args ), 0 );
    CHECK_EQ( args.owner, 7 );
    CHECK_EQ( poll1( m, 7 ), 0 );   /* not reported again */
    /* a wait with a mutex needs an owner */
    CHECK_EQ( poll1( m, 0 ), EINVAL );
    ios_usync_close( fds[0] );
    ios_usync_close( m );
}

struct waiter_arg
{
    int          fds[4];
    unsigned int count;
    int          all;
    unsigned int owner;
    __u64        timeout;
    int          alert;
    int          result;
    unsigned int index;
    volatile int done;
};

#define DONE( a )  __atomic_load_n( &(a).done, __ATOMIC_ACQUIRE )

static void *waiter_main( void *ptr )
{
    struct waiter_arg *a = ptr;
    a->result = do_wait( a->fds, a->count, a->all, a->owner, a->timeout, 0, a->alert, &a->index );
    __atomic_store_n( &a->done, 1, __ATOMIC_RELEASE );
    return NULL;
}

static void start_waiter( pthread_t *t, struct waiter_arg *a )
{
    a->done = 0;
    a->result = -1;
    pthread_create( t, NULL, waiter_main, a );
    sleep_ms( 30 );   /* long enough to be queued */
}

static void test_events(void)
{
    struct waiter_arg a1, a2, a3;
    pthread_t t1, t2, t3;
    __u32 prev;
    int e;

    e = mk_event( 1, 0 );
    CHECK_EQ( poll1( e, 1 ), ETIMEDOUT );
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_SET, &prev ), 0 );
    CHECK_EQ( prev, 0 );
    CHECK_EQ( poll1( e, 1 ), 0 );
    CHECK_EQ( poll1( e, 1 ), 0 );   /* manual: stays set */
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_RESET, &prev ), 0 );
    CHECK_EQ( prev, 1 );
    CHECK_EQ( poll1( e, 1 ), ETIMEDOUT );

    /* manual pulse: releases both current waiters, then it is reset */
    memset( &a1, 0, sizeof(a1) );
    a1.fds[0] = e; a1.count = 1; a1.owner = 11; a1.timeout = INF;
    a2 = a1; a2.owner = 12;
    start_waiter( &t1, &a1 );
    start_waiter( &t2, &a2 );
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_PULSE, &prev ), 0 );
    CHECK_EQ( prev, 0 );
    pthread_join( t1, NULL );
    pthread_join( t2, NULL );
    CHECK_EQ( a1.result, 0 );
    CHECK_EQ( a2.result, 0 );
    CHECK_EQ( event_state( e ), 0 );
    CHECK_EQ( poll1( e, 1 ), ETIMEDOUT );   /* a later waiter gets nothing */
    ios_usync_close( e );

    /* auto: one waiter per set; a pulse with nobody waiting releases nobody */
    e = mk_event( 0, 0 );
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_PULSE, &prev ), 0 );
    CHECK_EQ( poll1( e, 1 ), ETIMEDOUT );
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_SET, &prev ), 0 );
    CHECK_EQ( poll1( e, 1 ), 0 );
    CHECK_EQ( poll1( e, 1 ), ETIMEDOUT );   /* auto-reset */
    memset( &a1, 0, sizeof(a1) );
    a1.fds[0] = e; a1.count = 1; a1.owner = 11; a1.timeout = INF;
    a2 = a1; a2.owner = 12;
    a3 = a1; a3.owner = 13;
    start_waiter( &t1, &a1 );
    start_waiter( &t2, &a2 );
    start_waiter( &t3, &a3 );
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_PULSE, &prev ), 0 );
    sleep_ms( 30 );
    /* exactly one, and the first to arrive */
    CHECK( DONE( a1 ) && !DONE( a2 ) && !DONE( a3 ));
    CHECK_EQ( event_state( e ), 0 );
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_SET, &prev ), 0 );
    sleep_ms( 30 );
    CHECK( DONE( a2 ) && !DONE( a3 ));
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_SET, &prev ), 0 );
    pthread_join( t1, NULL );
    pthread_join( t2, NULL );
    pthread_join( t3, NULL );
    CHECK_EQ( a1.result, 0 );
    CHECK_EQ( a2.result, 0 );
    CHECK_EQ( a3.result, 0 );
    CHECK_EQ( event_state( e ), 0 );
    ios_usync_close( e );
}

static void test_wait_any_all(void)
{
    struct waiter_arg a;
    unsigned int index;
    pthread_t t;
    __u32 n;
    int fds[3];

    fds[0] = mk_sem( 1, 5 );
    fds[1] = mk_sem( 1, 5 );
    CHECK_EQ( do_wait( fds, 2, 0, 1, 0, 0, 0, &index ), 0 );
    CHECK_EQ( index, 0 );   /* the first ready object in array order */
    CHECK_EQ( do_wait( fds, 2, 0, 1, 0, 0, 0, &index ), 0 );
    CHECK_EQ( index, 1 );
    CHECK_EQ( do_wait( fds, 2, 0, 1, 0, 0, 0, &index ), ETIMEDOUT );

    /* wait-all: all or nothing */
    n = 1;
    ios_usync_ioctl( fds[0], NTSYNC_IOC_SEM_RELEASE, &n );
    CHECK_EQ( do_wait( fds, 2, 1, 1, 0, 0, 0, &index ), ETIMEDOUT );
    CHECK_EQ( sem_count( fds[0] ), 1 );
    CHECK_EQ( sem_count( fds[1] ), 0 );
    memset( &a, 0, sizeof(a) );
    a.fds[0] = fds[0]; a.fds[1] = fds[1]; a.count = 2; a.all = 1; a.owner = 3; a.timeout = INF;
    start_waiter( &t, &a );
    CHECK( !DONE( a ));
    n = 1;
    ios_usync_ioctl( fds[1], NTSYNC_IOC_SEM_RELEASE, &n );
    pthread_join( t, NULL );
    CHECK_EQ( a.result, 0 );
    CHECK_EQ( sem_count( fds[0] ), 0 );
    CHECK_EQ( sem_count( fds[1] ), 0 );
    /* duplicates: fine for wait-any, not for wait-all */
    fds[2] = fds[1];
    CHECK_EQ( do_wait( fds + 1, 2, 1, 1, 0, 0, 0, &index ), EINVAL );
    CHECK_EQ( do_wait( fds + 1, 2, 0, 1, 0, 0, 0, &index ), ETIMEDOUT );
    /* bad descriptors, too many objects, bad flags */
    fds[2] = (int)(IOS_USYNC_FD_TAG | 0x0ffffff0);
    CHECK_EQ( do_wait( fds, 3, 0, 1, 0, 0, 0, &index ), EBADF );
    {
        struct ntsync_wait_args args;
        memset( &args, 0, sizeof(args) );
        args.objs = (uintptr_t)fds;
        args.count = NTSYNC_MAX_WAIT_COUNT + 1;
        CHECK_EQ( ios_usync_ioctl( DEV, NTSYNC_IOC_WAIT_ANY, &args ), -1 );
        CHECK_EQ( errno, EINVAL );
        args.count = 1;
        args.flags = 2;
        CHECK_EQ( ios_usync_ioctl( DEV, NTSYNC_IOC_WAIT_ANY, &args ), -1 );
        CHECK_EQ( errno, EINVAL );
    }
    ios_usync_close( fds[0] );
    ios_usync_close( fds[1] );
}

static void test_alert(void)
{
    struct waiter_arg a;
    unsigned int index;
    pthread_t t;
    __u32 prev;
    int e, alert, s;

    e = mk_event( 0, 0 );
    alert = mk_event( 1, 1 );
    CHECK_EQ( do_wait( &e, 1, 0, 1, INF, 0, alert, &index ), 0 );
    CHECK_EQ( index, 1 );   /* count */
    CHECK_EQ( event_state( alert ), 1 );   /* manual: not consumed */
    /* an object that is ready comes first */
    ios_usync_ioctl( e, NTSYNC_IOC_EVENT_SET, &prev );
    CHECK_EQ( do_wait( &e, 1, 0, 1, INF, 0, alert, &index ), 0 );
    CHECK_EQ( index, 0 );
    /* wait-all ends on the alert too */
    s = mk_sem( 0, 1 );
    {
        int fds[2] = { e, s };
        CHECK_EQ( do_wait( fds, 2, 1, 1, INF, 0, alert, &index ), 0 );
        CHECK_EQ( index, 2 );
    }
    /* the alert fires while queued */
    ios_usync_ioctl( alert, NTSYNC_IOC_EVENT_RESET, &prev );
    memset( &a, 0, sizeof(a) );
    a.fds[0] = e; a.count = 1; a.owner = 2; a.timeout = INF; a.alert = alert;
    start_waiter( &t, &a );
    CHECK( !DONE( a ));
    ios_usync_ioctl( alert, NTSYNC_IOC_EVENT_SET, &prev );
    pthread_join( t, NULL );
    CHECK_EQ( a.result, 0 );
    CHECK_EQ( a.index, 1 );
    /* an alert that isn't an event; wait-all with the alert among the objects; owner 0 */
    CHECK_EQ( do_wait( &e, 1, 0, 1, 0, 0, s, &index ), EINVAL );
    CHECK_EQ( do_wait( &e, 1, 1, 1, 0, 0, e, &index ), EINVAL );
    CHECK_EQ( do_wait( &e, 1, 0, 0, 0, 0, 0, &index ), EINVAL );
    CHECK_EQ( do_wait( &e, 1, 0, 1, 0, 0, e, &index ), ETIMEDOUT );   /* wait-any allows it */
    ios_usync_close( e );
    ios_usync_close( alert );
    ios_usync_close( s );
}

static void test_timeouts(void)
{
    uint64_t start, end;
    int e = mk_event( 0, 0 );

    start = now_ns( CLOCK_MONOTONIC );
    CHECK_EQ( do_wait( &e, 1, 0, 1, start + 20000000, 0, 0, NULL ), ETIMEDOUT );
    end = now_ns( CLOCK_MONOTONIC );
    CHECK( end - start >= 20000000 );
    CHECK( end - start < 2000000000 );

    start = now_ns( CLOCK_REALTIME );
    CHECK_EQ( do_wait( &e, 1, 0, 1, start + 20000000, 1, 0, NULL ), ETIMEDOUT );
    end = now_ns( CLOCK_REALTIME );
    CHECK( end - start >= 20000000 );
    CHECK( end - start < 2000000000 );

    /* a past realtime deadline is a poll */
    CHECK_EQ( do_wait( &e, 1, 0, 1, 1, 1, 0, NULL ), ETIMEDOUT );
    ios_usync_close( e );
}

static void test_close_during_wait(void)
{
    struct ntsync_sem_args args;
    struct waiter_arg a;
    pthread_t t;
    __u32 n = 1;
    int s = mk_sem( 0, 1 );

    memset( &a, 0, sizeof(a) );
    a.fds[0] = s; a.count = 1; a.owner = 1; a.timeout = INF;
    start_waiter( &t, &a );
    CHECK_EQ( ios_usync_close( s ), 0 );   /* the creator's reference */
    /* the wait's reference keeps the object */
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_SEM_RELEASE, &n ), 0 );
    pthread_join( t, NULL );
    CHECK_EQ( a.result, 0 );
    /* now the last reference is gone */
    CHECK_EQ( ios_usync_ioctl( s, NTSYNC_IOC_SEM_READ, &args ), -1 );
    CHECK_EQ( errno, EBADF );
}

static void *cancel_main( void *ptr )
{
    struct waiter_arg *a = ptr;
    a->result = do_wait( a->fds, a->count, 0, a->owner, INF, 0, 0, NULL );
    return NULL;
}

static void test_thread_ends_while_queued(void)
{
    struct ntsync_event_args args;
    struct waiter_arg a, b;
    pthread_t t, u;
    __u32 prev;
    int e = mk_event( 0, 0 );

    memset( &a, 0, sizeof(a) );
    a.fds[0] = e; a.count = 1; a.owner = 1;
    pthread_create( &t, NULL, cancel_main, &a );
    sleep_ms( 30 );
    pthread_cancel( t );   /* ends inside the sleep: the destructor unlinks it */
    pthread_join( t, NULL );

    /* the next waiter is first in line, the set goes to it */
    memset( &b, 0, sizeof(b) );
    b.fds[0] = e; b.count = 1; b.owner = 2; b.timeout = INF;
    start_waiter( &u, &b );
    ios_usync_ioctl( e, NTSYNC_IOC_EVENT_SET, &prev );
    pthread_join( u, NULL );
    CHECK_EQ( b.result, 0 );
    /* the dead waiter's reference was dropped: closing frees it */
    CHECK_EQ( ios_usync_close( e ), 0 );
    CHECK_EQ( ios_usync_ioctl( e, NTSYNC_IOC_EVENT_READ, &args ), -1 );
    CHECK_EQ( errno, EBADF );
}

static void test_handoff(void)
{
    struct ntsync_sem_args args;
    int s1 = mk_sem( 0, 1 ), s2 = mk_sem( 0, 1 ), i, fd;

    CHECK_EQ( ios_usync_take( 1, 2 ), -1 );   /* nothing posted: never blocks */
    CHECK_EQ( ios_usync_post( 1, 2, s1 ), 0 );
    CHECK_EQ( ios_usync_post( 1, 2, s2 ), 0 );   /* replaces the first, dropping its reference */
    CHECK_EQ( ios_usync_close( s1 ), 0 );
    CHECK_EQ( ios_usync_ioctl( s1, NTSYNC_IOC_SEM_READ, &args ), -1 );   /* gone */
    CHECK_EQ( ios_usync_take( 2, 2 ), -1 );      /* another process */
    CHECK_EQ( ios_usync_take( 1, 3 ), -1 );      /* another token */
    fd = ios_usync_take( 1, 2 );
    CHECK_EQ( fd, s2 );
    CHECK_EQ( ios_usync_take( 1, 2 ), -1 );
    /* the taken reference and the creator's */
    CHECK_EQ( ios_usync_close( s2 ), 0 );
    CHECK_EQ( ios_usync_ioctl( fd, NTSYNC_IOC_SEM_READ, &args ), 0 );
    CHECK_EQ( ios_usync_close( fd ), 0 );
    CHECK_EQ( ios_usync_ioctl( fd, NTSYNC_IOC_SEM_READ, &args ), -1 );
    CHECK_EQ( ios_usync_post( 1, 2, fd ), -1 );   /* a dead descriptor can't be posted */

    /* at least 256 pending posts */
    s1 = mk_sem( 0, 1 );
    for (i = 0; i < 256; i++) CHECK_EQ( ios_usync_post( 100, i, s1 ), 0 );
    CHECK_EQ( ios_usync_post( 100, 256, s1 ), -1 );
    CHECK_EQ( errno, ENOSPC );
    for (i = 0; i < 256; i++)
    {
        CHECK_EQ( ios_usync_take( 100, i ), s1 );
        ios_usync_close( s1 );
    }
    ios_usync_close( s1 );
    CHECK_EQ( ios_usync_ioctl( s1, NTSYNC_IOC_SEM_READ, &args ), -1 );
}

static void test_libc_passthrough(void)
{
    int fds[2];
    CHECK_EQ( pipe( fds ), 0 );
    CHECK_EQ( ios_usync_close( fds[0] ), 0 );
    CHECK_EQ( ios_usync_close( fds[0] ), -1 );   /* libc's close: already closed */
    CHECK_EQ( errno, EBADF );
    close( fds[1] );
}

/* ---- stress: tokens are conserved ---- */

#define STRESS_THREADS  8
#define STRESS_ROUNDS   3000

static int stress_sem, stress_mutex, stress_event, stress_alert;
static long stress_consumed, stress_counter, stress_alerts;

static void *stress_main( void *ptr )
{
    unsigned int owner = (unsigned int)(uintptr_t)ptr, index, i;
    int fds[3] = { stress_sem, stress_event, stress_mutex };

    for (i = 0; i < STRESS_ROUNDS; i++)
    {
        uint64_t deadline = now_ns( CLOCK_MONOTONIC ) + 2000000;
        int r = do_wait( fds, 2, 0, owner, (i % 3) ? deadline : 0, 0, stress_alert, &index );
        __u32 n = 1, prev;

        if (!r && index == 0) __atomic_add_fetch( &stress_consumed, 1, __ATOMIC_RELAXED );
        if (!r && index == 1) ios_usync_ioctl( stress_event, NTSYNC_IOC_EVENT_RESET, &prev );
        if (!r && index == 2) __atomic_add_fetch( &stress_alerts, 1, __ATOMIC_RELAXED );
        if (i % 4 == 0) ios_usync_ioctl( stress_sem, NTSYNC_IOC_SEM_RELEASE, &n );
        if (i % 7 == 0) ios_usync_ioctl( stress_event, NTSYNC_IOC_EVENT_PULSE, &prev );

        /* the mutex guards a plain counter */
        CHECK_EQ( do_wait( &stress_mutex, 1, 0, owner, INF, 0, 0, NULL ), 0 );
        stress_counter++;
        {
            struct ntsync_mutex_args args = { owner, 0 };
            CHECK_EQ( ios_usync_ioctl( stress_mutex, NTSYNC_IOC_MUTEX_UNLOCK, &args ), 0 );
        }
    }
    return NULL;
}

static void test_stress(void)
{
    pthread_t threads[STRESS_THREADS];
    long released = (long)STRESS_THREADS * ((STRESS_ROUNDS + 3) / 4);
    __u32 prev;
    int i;

    stress_sem = mk_sem( 0, 0x7fffffff );
    stress_mutex = mk_mutex( 0, 0 );
    stress_event = mk_event( 0, 0 );
    stress_alert = mk_event( 1, 0 );
    for (i = 0; i < STRESS_THREADS; i++) pthread_create( &threads[i], NULL, stress_main, (void *)(uintptr_t)(i + 1) );
    for (i = 0; i < STRESS_THREADS; i++) pthread_join( threads[i], NULL );
    CHECK_EQ( stress_counter, (long)STRESS_THREADS * STRESS_ROUNDS );
    CHECK_EQ( stress_alerts, 0 );
    CHECK_EQ( stress_consumed + sem_count( stress_sem ), released );
    /* drain what is left: still conserved */
    while (!poll1( stress_sem, 1 )) stress_consumed++;
    CHECK_EQ( stress_consumed, released );
    ios_usync_ioctl( stress_event, NTSYNC_IOC_EVENT_RESET, &prev );
    ios_usync_close( stress_sem );
    ios_usync_close( stress_mutex );
    ios_usync_close( stress_event );
    ios_usync_close( stress_alert );
}

/* many objects: the table grows, slots are reused */
static void test_table(void)
{
    static int fds[10000];
    int i, again;

    for (i = 0; i < 10000; i++)
    {
        fds[i] = mk_event( 0, i & 1 );
        CHECK( fds[i] > 0 );
    }
    for (i = 0; i < 10000; i += 997) CHECK_EQ( event_state( fds[i] ), i & 1 );
    for (i = 0; i < 10000; i++) ios_usync_close( fds[i] );
    again = mk_event( 0, 0 );
    CHECK( ((unsigned int)again & 0x0fffffffu) < 10010 );   /* a freed slot */
    ios_usync_close( again );
}

int main(void)
{
    test_semaphore();
    test_mutex();
    test_events();
    test_wait_any_all();
    test_alert();
    test_timeouts();
    test_close_during_wait();
    test_thread_ends_while_queued();
    test_handoff();
    test_libc_passthrough();
    test_table();
    test_stress();
    return check_done( "test_usync" );
}
