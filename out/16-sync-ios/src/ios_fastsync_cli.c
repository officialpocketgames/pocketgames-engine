// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part D: the client's side of fastsync, minus the server requests (those stay in spec
// 09's sync.c): the handle -> cell cache (D1), the gates (D2), the operations on a cell (D3-D7),
// the fast wait's spin and park (D6), and the small arithmetic of D6-D8.

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "ios_sync.h"

uint64_t ios_fs_now_ns(void)
{
#ifdef __APPLE__
    return clock_gettime_nsec_np( CLOCK_UPTIME_RAW );   /* matches the park's clock, stops in sleep */
#else
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
#endif
}

static inline void cpu_pause(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__( "isb" ::: "memory" );
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

/* ======== D1: the cache ======== */

struct cache_slot
{
    uint32_t seq;      /* odd while a writer is in it */
    uint32_t handle;   /* 0: empty */
    uint32_t pid;
    uint32_t access;
    uint32_t gen;
    int32_t  index;
    uint32_t manual;
    uint32_t pad;
};

static struct cache_slot cache[IOS_FS_CACHE_ENTRIES];
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct cache_slot *slot_for( uint32_t handle )
{
    return &cache[(handle >> 2) % IOS_FS_CACHE_ENTRIES];
}

#define LOAD(field)      __atomic_load_n( &(field), __ATOMIC_RELAXED )
#define STORE(field, v)  __atomic_store_n( &(field), (v), __ATOMIC_RELAXED )

int ios_fs_cache_lookup( uint32_t handle, uint32_t pid, struct ios_fs_entry *out )
{
    struct cache_slot *slot = slot_for( handle );
    struct ios_fs_entry e;
    unsigned int tries;

    for (tries = 0; tries < 256; tries++)
    {
        uint32_t s1 = __atomic_load_n( &slot->seq, __ATOMIC_ACQUIRE ), s2;

        if (s1 & 1)
        {
            cpu_pause();
            continue;
        }
        e.handle = LOAD( slot->handle );
        e.pid    = LOAD( slot->pid );
        e.access = LOAD( slot->access );
        e.gen    = LOAD( slot->gen );
        e.index  = LOAD( slot->index );
        e.manual = LOAD( slot->manual );
        __atomic_thread_fence( __ATOMIC_ACQUIRE );
        s2 = __atomic_load_n( &slot->seq, __ATOMIC_RELAXED );
        if (s1 != s2) continue;
        if (!e.handle || e.handle != handle || e.pid != pid) return 0;
        *out = e;
        return 1;
    }
    return 0;   /* a busy slot: treat as a miss */
}

/* writers: one mutex, signals blocked (upstream's uninterrupted section) */
static void writer_enter( int signals_blocked, sigset_t *old )
{
    if (!signals_blocked)
    {
        sigset_t all;
        sigfillset( &all );
        pthread_sigmask( SIG_BLOCK, &all, old );
    }
    pthread_mutex_lock( &cache_mutex );
}

static void writer_leave( int signals_blocked, const sigset_t *old )
{
    pthread_mutex_unlock( &cache_mutex );
    if (!signals_blocked) pthread_sigmask( SIG_SETMASK, old, NULL );
}

static void slot_write( struct cache_slot *slot, const struct ios_fs_entry *e )
{
    uint32_t seq = LOAD( slot->seq );

    STORE( slot->seq, seq + 1 );
    __atomic_thread_fence( __ATOMIC_RELEASE );   /* after the odd store */
    STORE( slot->handle, e ? e->handle : 0 );
    STORE( slot->pid,    e ? e->pid : 0 );
    STORE( slot->access, e ? e->access : 0 );
    STORE( slot->gen,    e ? e->gen : 0 );
    STORE( slot->index,  e ? e->index : IOS_FS_NOT_FAST );
    STORE( slot->manual, e ? e->manual : 0 );
    __atomic_store_n( &slot->seq, seq + 2, __ATOMIC_RELEASE );   /* fence before the even one */
}

void ios_fs_cache_store( const struct ios_fs_entry *entry, int signals_blocked )
{
    sigset_t old;

    if (!entry->handle) return;
    writer_enter( signals_blocked, &old );
    slot_write( slot_for( entry->handle ), entry );
    writer_leave( signals_blocked, &old );
}

void ios_fs_cache_evict_handle( uint32_t handle, int signals_blocked )
{
    struct cache_slot *slot = slot_for( handle );
    sigset_t old;

    if (!handle || LOAD( slot->handle ) != handle) return;   /* the common case: nothing cached */
    writer_enter( signals_blocked, &old );
    if (LOAD( slot->handle ) == handle) slot_write( slot, NULL );   /* whatever its process */
    writer_leave( signals_blocked, &old );
}

void ios_fs_cache_evict_pid( uint32_t pid, int signals_blocked )
{
    unsigned int i;
    sigset_t old;

    writer_enter( signals_blocked, &old );
    for (i = 0; i < IOS_FS_CACHE_ENTRIES; i++)
        if (LOAD( cache[i].handle ) && LOAD( cache[i].pid ) == pid) slot_write( &cache[i], NULL );
    writer_leave( signals_blocked, &old );
}

void ios_fs_process_init( uint32_t pid, int device_in_use )
{
    static const char *const modes[] = { "off", "on", "auto", "cells" };
    static int logged_line, logged_other;
    const struct ios_fs_config *config = ios_fs_config();

    /* a dead process may have left entries under this id */
    ios_fs_cache_evict_pid( pid, 0 );
    ios_usync_engine_recheck();
    if (device_in_use)
    {
        ios_fs_set_other_engine( 1 );
        if (config->mode != IOS_FS_OFF && !__atomic_exchange_n( &logged_other, 1, __ATOMIC_RELAXED ))
            ios_sync_log( "fastsync: not started, in-process ntsync owns the session" );
        return;
    }
    ios_fs_config_recheck();
    if (config->mode != IOS_FS_OFF && !__atomic_exchange_n( &logged_line, 1, __ATOMIC_RELAXED ))
        ios_sync_log( "fastsync: mode %s, poll peek %s, semaphores %s, %d cells, park cap %u us, "
                      "spin ceiling %u us, auto threshold %u",
                      modes[config->mode], config->pollpeek ? "on" : "off", config->sem ? "on" : "off",
                      IOS_SYNC_CELLS, config->cap_us, config->spin_us, config->auto_reqs );
}

int ios_fs_reply_index( uint32_t type, int32_t *index, uint32_t *manual )
{
    uint32_t idx;

    if (!(type & IOS_FASTSYNC_REPLY_CELL)) return 0;
    idx = type & IOS_FASTSYNC_INDEX_MASK;
    if (idx >= IOS_SYNC_CELLS) return 0;   /* never trust an index from the wire */
    *index = (int32_t)idx;
    *manual = !!(type & IOS_FASTSYNC_REPLY_MANUAL);
    return 1;
}

int ios_fs_entry_usable( const struct ios_fs_entry *entry, uint32_t access )
{
    if (entry->index < 0) return 0;   /* known not to be a fast object */
    if ((entry->access & access) != access) return 0;   /* the server reports the error */
    /* a semaphore cell while semaphores are off: the server */
    if (__atomic_load_n( &ios_cell_at( entry->index )->kind, __ATOMIC_RELAXED ) == IOS_CELL_SEMAPHORE &&
        !ios_fs_config()->sem)
        return 0;
    return 1;
}

/* ======== D2: gates ======== */

static int other_engine;
static int armed;
static uint64_t unarmed_count;
static int auto_lock;
static struct ios_fs_auto auto_state;

void ios_fs_set_other_engine( int on )
{
    __atomic_store_n( &other_engine, !!on, __ATOMIC_RELAXED );
}

int ios_fs_other_engine(void)
{
    return __atomic_load_n( &other_engine, __ATOMIC_RELAXED );
}

int ios_fs_cells_on(void)
{
    return !ios_fs_other_engine() && ios_fs_config()->mode != IOS_FS_OFF;
}

int ios_fs_armed(void)
{
    const struct ios_fs_config *config;

    if (ios_fs_other_engine()) return 0;
    config = ios_fs_config();
    if (config->mode == IOS_FS_ON) return 1;
    if (config->mode != IOS_FS_AUTO) return 0;
    return __atomic_load_n( &armed, __ATOMIC_ACQUIRE );
}

#define AUTO_WINDOW_NS  (10ull * 1000000000ull)

int ios_fs_auto_check( struct ios_fs_auto *st, uint64_t count, uint64_t now_ns,
                       unsigned int threshold, uint64_t *elapsed_ns, uint64_t *in_window )
{
    if (!st->started)
    {
        st->started = 1;
        st->window_start_ns = now_ns;
        st->window_start_count = 0;
    }
    *elapsed_ns = now_ns - st->window_start_ns;
    *in_window = count - st->window_start_count;
    if (*elapsed_ns <= AUTO_WINDOW_NS && *in_window >= threshold) return 1;
    if (*elapsed_ns > AUTO_WINDOW_NS)
    {
        /* the window ended below the threshold: start counting again */
        st->window_start_ns = now_ns;
        st->window_start_count = count;
    }
    return 0;
}

void ios_fs_count_unarmed(void)
{
    const struct ios_fs_config *config = ios_fs_config();
    uint64_t n, elapsed, in_window;

    if (config->mode != IOS_FS_AUTO || ios_fs_other_engine()) return;
    if (__atomic_load_n( &armed, __ATOMIC_RELAXED )) return;
    n = __atomic_add_fetch( &unarmed_count, 1, __ATOMIC_RELAXED );
    if (n % 256) return;
    if (__atomic_exchange_n( &auto_lock, 1, __ATOMIC_ACQUIRE )) return;   /* another thread checks */
    if (ios_fs_auto_check( &auto_state, n, ios_fs_now_ns(), config->auto_reqs, &elapsed, &in_window ) &&
        !__atomic_exchange_n( &armed, 1, __ATOMIC_RELEASE ))
        ios_sync_log( "fastsync: armed after %llu operations in %llu.%03llu s",
                      (unsigned long long)in_window, (unsigned long long)(elapsed / 1000000000u),
                      (unsigned long long)(elapsed / 1000000u % 1000u) );
    __atomic_store_n( &auto_lock, 0, __ATOMIC_RELEASE );
}

/* ======== D3-D7: operations on a cell ======== */

static inline uint32_t cell_kind( const struct ios_sync_cell *c )
{
    return __atomic_load_n( &c->kind, __ATOMIC_RELAXED );
}

static inline uint32_t cell_manual( const struct ios_sync_cell *c )
{
    return __atomic_load_n( &c->manual, __ATOMIC_RELAXED );
}

int ios_fs_cli_event_change( int cell, uint32_t gen, int set, int *prev_state, int *wake_server )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    int32_t target = set ? IOS_CELL_SET : IOS_CELL_RESET, state;
    uint64_t word = ios_cell_load( c );

    *wake_server = 0;
    for (;;)
    {
        if (ios_cell_gen( word ) != gen) return IOS_FS_GO_SERVER;
        /* the generation holds, so kind and manual are this object's */
        if (cell_kind( c ) != IOS_CELL_EVENT) return IOS_FS_GO_SERVER;
        state = ios_cell_state( word );
        /* CLAIMED: the server is between signaled and satisfied; its request is right */
        if (state == IOS_CELL_DISABLED || state == IOS_CELL_CLAIMED) return IOS_FS_GO_SERVER;
        if (state == target)
        {
            *prev_state = state != IOS_CELL_RESET;
            return 0;
        }
        if (ios_cell_cas( c, &word, ios_cell_make( gen, target ))) break;
    }
    *prev_state = state != IOS_CELL_RESET;
    if (set)
    {
        if (ios_cell_cli_waiters( c )) ios_cell_wake( c, cell_manual( c ));
        *wake_server = ios_cell_srv_waiters( c ) != 0;
    }
    return 0;
}

int ios_fs_cli_sem_release( int cell, uint32_t gen, unsigned int count, unsigned int *prev,
                            int *wake_server )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );
    uint32_t max;
    int32_t state;

    *wake_server = 0;
    for (;;)
    {
        if (ios_cell_gen( word ) != gen) return IOS_FS_GO_SERVER;
        if (cell_kind( c ) != IOS_CELL_SEMAPHORE) return IOS_FS_GO_SERVER;
        max = __atomic_load_n( &c->max, __ATOMIC_RELAXED );
        if (max < 1 || max > 0x7fffffff) return IOS_FS_GO_SERVER;
        state = ios_cell_state( word );
        if (state < 0) return IOS_FS_GO_SERVER;
        if (count > max || (uint64_t)state + count > max) return 1;   /* nothing changes */
        if (ios_cell_cas( c, &word, ios_cell_make( gen, state + (int32_t)count ))) break;
    }
    *prev = (unsigned int)state;
    if (count)
    {
        if (ios_cell_cli_waiters( c )) ios_cell_wake( c, count > 1 );
        *wake_server = ios_cell_srv_waiters( c ) != 0;
    }
    return 0;
}

int ios_fs_cli_sem_query( int cell, uint32_t gen, unsigned int *count, unsigned int *max )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );

    if (ios_cell_gen( word ) != gen || cell_kind( c ) != IOS_CELL_SEMAPHORE) return IOS_FS_GO_SERVER;
    if (ios_cell_state( word ) < 0) return IOS_FS_GO_SERVER;
    *count = (unsigned int)ios_cell_state( word );
    *max = __atomic_load_n( &c->max, __ATOMIC_RELAXED );
    return 0;
}

int ios_fs_cli_try( int cell, uint32_t gen )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );

    for (;;)
    {
        int32_t state;

        if (ios_cell_gen( word ) != gen) return IOS_FS_GO_SERVER;
        state = ios_cell_state( word );
        if (state == IOS_CELL_DISABLED) return IOS_FS_GO_SERVER;
        if (cell_kind( c ) == IOS_CELL_SEMAPHORE)
        {
            if (state < 0) return IOS_FS_GO_SERVER;
            if (!state) return 0;
            if (ios_cell_cas( c, &word, ios_cell_make( gen, state - 1 ))) return 1;
        }
        else if (cell_manual( c ))
        {
            if (state == IOS_CELL_SET) return 1;   /* without consuming */
            if (state == IOS_CELL_RESET) return 0;
            return IOS_FS_GO_SERVER;
        }
        else
        {
            if (state == IOS_CELL_RESET) return 0;
            if (state != IOS_CELL_SET) return IOS_FS_GO_SERVER;   /* CLAIMED: the server's */
            if (ios_cell_cas( c, &word, ios_cell_make( gen, IOS_CELL_RESET ))) return 1;
        }
    }
}

int ios_fs_cli_peek_unsignaled( int cell, uint32_t gen )
{
    uint64_t word = ios_cell_load( ios_cell_at( cell ));
    /* RESET for an event, empty for a semaphore: both 0; never writes */
    return ios_cell_gen( word ) == gen && ios_cell_state( word ) == 0;
}

int ios_fs_cli_reads_signaled( int cell, uint32_t gen )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );

    if (ios_cell_gen( word ) != gen) return 0;
    return ios_cell_client_signaled( cell_kind( c ), cell_manual( c ), ios_cell_state( word ));
}

/* ======== D6: spin credit ======== */

uint64_t ios_fs_spin_budget( struct ios_fs_spin *st, uint64_t ceiling_ns, int *probe )
{
    int credit = __atomic_load_n( &st->credit, __ATOMIC_RELAXED );

    *probe = 0;
    if (credit > 0) return ceiling_ns * (unsigned int)credit / IOS_FS_CREDIT_MAX;
    /* one wait in 64 still spins a little, so spinning comes back when it pays again */
    if (__atomic_fetch_add( &st->probe, 1, __ATOMIC_RELAXED ) % 64) return 0;
    *probe = 1;
    return ceiling_ns / 8;
}

void ios_fs_spin_result( struct ios_fs_spin *st, int success, int probe )
{
    int credit = __atomic_load_n( &st->credit, __ATOMIC_RELAXED ), next;

    do
    {
        if (success && probe) next = credit > 16 ? credit : 16;
        else if (success) next = credit + 2 > IOS_FS_CREDIT_MAX ? IOS_FS_CREDIT_MAX : credit + 2;
        else next = credit > 0 ? credit - 1 : 0;
    } while (!__atomic_compare_exchange_n( &st->credit, &credit, next, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED ));
}

/* ======== D6: the fast wait ======== */

#define SPIN_TRIES   96
#define PARK_ROUNDS  64

static struct ios_fs_spin spin_state = { IOS_FS_CREDIT_MAX, 0 };

/* 96 tries separated by a pause: 1 took, 0 nothing, -1 stop */
static int spin_block( int cell, uint32_t gen )
{
    int i, r;

    for (i = 0; i < SPIN_TRIES; i++)
    {
        cpu_pause();
        if ((r = ios_fs_cli_try( cell, gen ))) return r;
    }
    return 0;
}

int ios_fs_cli_wait( int cell, uint32_t gen, uint64_t budget_ns, uint64_t *spent_ns, uint64_t *charge_cap_ns )
{
    const struct ios_fs_config *config = ios_fs_config();
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t start = ios_fs_now_ns(), now, spin_ns, park_ns = 0, park_start = 0, deadline;
    int r, probe, round, result = 0;

    if ((r = ios_fs_cli_try( cell, gen )) || (r = spin_block( cell, gen )))
    {
        result = r > 0;
        goto done;
    }

    /* the timed spin: the credit's budget, never past the caller's timeout */
    spin_ns = ios_fs_spin_budget( &spin_state, (uint64_t)config->spin_us * 1000, &probe );
    now = ios_fs_now_ns();
    if (budget_ns != UINT64_MAX)
    {
        uint64_t left = now - start < budget_ns ? budget_ns - (now - start) : 0;
        if (spin_ns > left) spin_ns = left;
    }
    if (spin_ns)
    {
        deadline = now + spin_ns;
        do r = spin_block( cell, gen );
        while (!r && ios_fs_now_ns() < deadline);   /* also stops when the cell is recycled */
        if (r >= 0) ios_fs_spin_result( &spin_state, r > 0, probe );
        if (r)
        {
            result = r > 0;
            goto done;
        }
    }

    /* park: the smaller of the cap and the caller's remaining time */
    park_start = ios_fs_now_ns();
    park_ns = (uint64_t)config->cap_us * 1000;
    if (budget_ns != UINT64_MAX)
    {
        uint64_t used = park_start - start;
        if (used >= budget_ns) park_ns = 0;
        else if (budget_ns - used < park_ns) park_ns = budget_ns - used;
    }
    deadline = park_start + park_ns;
    for (round = 0; park_ns && round < PARK_ROUNDS; round++)   /* a stopped clock can't loop forever */
    {
        uint64_t word;
        int32_t state;

        ios_cell_add_cli_waiters( c, 1 );
        word = ios_cell_load( c );
        if (ios_cell_gen( word ) != gen) break;   /* the count belongs to the new occupant */
        if (ios_cell_state( word ) == 0)   /* RESET, or an empty semaphore */
        {
            now = ios_fs_now_ns();
            if (now >= deadline)
            {
                ios_cell_add_cli_waiters( c, -1 );
                break;
            }
            ios_cell_park( c, 0, deadline - now );
            word = ios_cell_load( c );
            if (ios_cell_gen( word ) != gen) break;   /* without decrementing */
        }
        ios_cell_add_cli_waiters( c, -1 );
        state = ios_cell_state( word );
        if (state < 0 || (cell_kind( c ) == IOS_CELL_EVENT && state == IOS_CELL_CLAIMED)) break;
        if ((r = ios_fs_cli_try( cell, gen )))
        {
            result = r > 0;
            break;
        }
        if (ios_fs_now_ns() >= deadline) break;
    }

done:
    *spent_ns = ios_fs_now_ns() - start;
    /* a relative timeout is charged the spin as measured and the park at most its budget, so a
     * clock that overshoots can't turn the rest into 0 (which the server treats as a poll) */
    *charge_cap_ns = park_start ? (park_start - start) + park_ns : *spent_ns;
    return result;
}

int64_t ios_fs_remaining_100ns( int64_t timeout_100ns, uint64_t elapsed_ns, uint64_t charge_cap_ns )
{
    uint64_t charged = elapsed_ns < charge_cap_ns ? elapsed_ns : charge_cap_ns;
    int64_t left = timeout_100ns - (int64_t)(charged / 100);

    return left > 0 ? left : 0;
}

/* ======== D7, D8 ======== */

int ios_fs_peek_to_server( struct ios_fs_peek *st )
{
    return !(++st->polls % 16);
}

int ios_fs_peek_answered( struct ios_fs_peek *st )
{
    return !(++st->peeks % 64);
}

unsigned int ios_fs_watchdog_next( unsigned int prev_seconds )
{
    if (!prev_seconds) return 2;
    return prev_seconds >= 15 ? 60 : prev_seconds * 4;
}
