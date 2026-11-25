// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 4, item 1.2-1.3: owner tracking for the unix-side locks every pseudo-process
// shares (see ios_lock.h). Darwin has no pthread_mutex_timedlock, so a contended lock is polled:
// try, yield a while, then sleep in steps of 50 us up to 1 ms. Each tracked lock has a slot that
// only its holder writes (it holds the lock); waiters read it to say who holds the lock.

#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   /* usleep, clock_gettime, PTHREAD_MUTEX_RECURSIVE under -std=c11 (host tests) */
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include "ios_lock.h"

#define IOS_LOCK_SLOTS   256
#define IOS_LOCK_SPINS   200

enum { OWNER_RUNNING, OWNER_SUSPENDED, OWNER_ENDED };

struct lock_slot
{
    pthread_mutex_t *lock;      /* claimed once, never given back */
    const char      *name;
    unsigned int     tid, pid, serial;  /* the holder */
    unsigned int     depth;     /* 0: free; more than 1: a recursive lock taken again */
    unsigned int     gen;       /* a new holder each time */
    int              state;     /* OWNER_* */
};

static struct lock_slot slots[IOS_LOCK_SLOTS];
static ios_lock_who_fn who_fn;
static ios_lock_log_fn log_fn;
static unsigned int wait_log_ms = 3000;
static pthread_mutex_t recover_mutex = PTHREAD_MUTEX_INITIALIZER;

void ios_lock_setup( ios_lock_who_fn who, ios_lock_log_fn log, unsigned int log_ms )
{
    who_fn = who;
    log_fn = log;
    wait_log_ms = log_ms ? log_ms : 3000;
}

void ios_lock_reset( void )
{
    memset( slots, 0, sizeof(slots) );
}

static void lock_log( const char *line )
{
    if (log_fn) log_fn( line );
    else fputs( line, stderr );
}

static unsigned long long now_ms( void )
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the slot of a lock, claimed when claim is set; NULL when the table is full */
static struct lock_slot *slot_of( pthread_mutex_t *mutex, int claim )
{
    unsigned int i, start = (unsigned int)(((uintptr_t)mutex >> 4) % IOS_LOCK_SLOTS);

    for (i = 0; i < IOS_LOCK_SLOTS; i++)
    {
        struct lock_slot *s = &slots[(start + i) % IOS_LOCK_SLOTS];
        pthread_mutex_t *cur = __atomic_load_n( &s->lock, __ATOMIC_ACQUIRE );

        if (cur == mutex) return s;
        if (cur) continue;
        if (!claim) return NULL;
        if (__atomic_compare_exchange_n( &s->lock, &cur, mutex, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
            return s;
        if (cur == mutex) return s;
    }
    return NULL;
}

void ios_lock_name_set( pthread_mutex_t *mutex, const char *name )
{
    struct lock_slot *s = slot_of( mutex, 1 );

    if (s && name) __atomic_store_n( &s->name, name, __ATOMIC_RELEASE );
}

static const char *slot_name( struct lock_slot *s, pthread_mutex_t *mutex, char *buf, size_t len )
{
    const char *name = s ? __atomic_load_n( &s->name, __ATOMIC_ACQUIRE ) : NULL;

    if (name) return name;
    snprintf( buf, len, "lock %p", (void *)mutex );
    return buf;
}

static const char *state_name( int state )
{
    switch (state)
    {
    case OWNER_ENDED:     return "ended";
    case OWNER_SUSPENDED: return "suspended";
    default:              return "running";
    }
}

static void who_am_i( struct ios_lock_who *me, int *known )
{
    if (*known) return;
    *known = 1;
    if (who_fn) who_fn( me );
}

/* the calling thread now holds the lock */
static void got( struct lock_slot *s, struct ios_lock_who *me, int *known )
{
    if (!s) return;
    if (s->depth) { s->depth++; return; }   /* only the holder can get here with a depth */
    who_am_i( me, known );
    __atomic_store_n( &s->tid, me->tid, __ATOMIC_RELAXED );
    __atomic_store_n( &s->pid, me->pid, __ATOMIC_RELAXED );
    __atomic_store_n( &s->serial, me->serial, __ATOMIC_RELAXED );
    __atomic_store_n( &s->state, OWNER_RUNNING, __ATOMIC_RELAXED );
    __atomic_add_fetch( &s->gen, 1, __ATOMIC_RELAXED );
    __atomic_store_n( &s->depth, 1, __ATOMIC_RELEASE );
}

/* the holder's process has ended: the lock starts again, taken by the caller */
static int recover( pthread_mutex_t *mutex, struct lock_slot *s, unsigned int gen, const char *name )
{
    pthread_mutexattr_t attr;
    char line[256];
    int ok;

    pthread_mutex_lock( &recover_mutex );
    ok = __atomic_load_n( &s->gen, __ATOMIC_ACQUIRE ) == gen && __atomic_load_n( &s->depth, __ATOMIC_ACQUIRE ) &&
         __atomic_load_n( &s->state, __ATOMIC_ACQUIRE ) == OWNER_ENDED;
    if (ok)
    {
        pthread_mutexattr_init( &attr );
        pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
        pthread_mutex_init( mutex, &attr );
        pthread_mutexattr_destroy( &attr );
        __atomic_store_n( &s->depth, 0, __ATOMIC_RELEASE );
        ok = !pthread_mutex_trylock( mutex );
        snprintf( line, sizeof(line), "[lock] %s was held by an ended process: released\n", name );
        lock_log( line );
    }
    pthread_mutex_unlock( &recover_mutex );
    return ok;
}

void ios_lock_take( pthread_mutex_t *mutex, const char *name )
{
    struct ios_lock_who me = { 0, 0, 0 };
    struct lock_slot *s = slot_of( mutex, 1 );
    unsigned long long start = 0;
    unsigned int i, delay = 50;
    int logged = 0, known = 0;
    char buf[48], line[512];

    if (s && name && !__atomic_load_n( &s->name, __ATOMIC_ACQUIRE )) __atomic_store_n( &s->name, name, __ATOMIC_RELEASE );

    for (i = 0; pthread_mutex_trylock( mutex ); i++)
    {
        unsigned int tid, pid, gen, depth;
        int state;

        if (i < IOS_LOCK_SPINS) { sched_yield(); continue; }
        if (i == IOS_LOCK_SPINS) start = now_ms();
        else if (now_ms() - start >= wait_log_ms)
        {
            who_am_i( &me, &known );
            name = slot_name( s, mutex, buf, sizeof(buf) );
            if (!s)
            {
                if (!logged++)
                {
                    snprintf( line, sizeof(line), "[lock] %s waited %u s by thread %04x of process %04x; "
                              "held by an untracked thread\n", name, (wait_log_ms + 500) / 1000, me.tid, me.pid );
                    lock_log( line );
                }
            }
            else
            {
                gen   = __atomic_load_n( &s->gen, __ATOMIC_ACQUIRE );
                depth = __atomic_load_n( &s->depth, __ATOMIC_ACQUIRE );
                tid   = __atomic_load_n( &s->tid, __ATOMIC_RELAXED );
                pid   = __atomic_load_n( &s->pid, __ATOMIC_RELAXED );
                state = __atomic_load_n( &s->state, __ATOMIC_RELAXED );
                if (!logged++)
                {
                    snprintf( line, sizeof(line), "[lock] %s waited %u s by thread %04x of process %04x; "
                              "held by thread %04x of process %04x (%s)\n", name, (wait_log_ms + 500) / 1000,
                              me.tid, me.pid, tid, pid, state_name( state ));
                    lock_log( line );
                }
                if (depth && state == OWNER_ENDED && recover( mutex, s, gen, name )) break;
            }
        }
        usleep( delay );
        if (delay < 1000) delay *= 2;
    }
    got( s, &me, &known );
}

void ios_lock_give( pthread_mutex_t *mutex )
{
    struct lock_slot *s = slot_of( mutex, 0 );

    if (s && s->depth) __atomic_store_n( &s->depth, s->depth - 1, __ATOMIC_RELEASE );
    pthread_mutex_unlock( mutex );
}

void ios_lock_process_ended( unsigned int serial )
{
    unsigned int i;

    if (!serial) return;
    for (i = 0; i < IOS_LOCK_SLOTS; i++)
        if (__atomic_load_n( &slots[i].depth, __ATOMIC_ACQUIRE ) &&
            __atomic_load_n( &slots[i].serial, __ATOMIC_RELAXED ) == serial)
            __atomic_store_n( &slots[i].state, OWNER_ENDED, __ATOMIC_RELEASE );
}

void ios_lock_thread_suspended( unsigned int serial, unsigned int tid )
{
    unsigned int i;

    for (i = 0; i < IOS_LOCK_SLOTS; i++)
        if (__atomic_load_n( &slots[i].depth, __ATOMIC_ACQUIRE ) &&
            __atomic_load_n( &slots[i].serial, __ATOMIC_RELAXED ) == serial &&
            __atomic_load_n( &slots[i].tid, __ATOMIC_RELAXED ) == tid &&
            __atomic_load_n( &slots[i].state, __ATOMIC_RELAXED ) != OWNER_ENDED)
            __atomic_store_n( &slots[i].state, OWNER_SUSPENDED, __ATOMIC_RELEASE );
}
