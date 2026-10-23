// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part C: the server's side of a cell. The server's event and semaphore code (spec 08's
// event.c and semaphore.c) calls these from its object hooks; they run on the server thread only,
// so the only concurrent writers of a cell are clients (Part D), which never write CLAIMED, never
// touch a DISABLED cell and never change the generation.
//
// A return of -1 means the cell is DISABLED: the object has left the fast path for good and the
// server's own field is authoritative again.

#include "ios_sync.h"

static inline uint32_t cell_manual( const struct ios_sync_cell *cell )
{
    return __atomic_load_n( &cell->manual, __ATOMIC_RELAXED );
}

static void wake_clients( struct ios_sync_cell *cell, int all )
{
    if (ios_cell_cli_waiters( cell )) ios_cell_wake( cell, all );
}

/* ---- C1 / C2: allocation ---- */

int ios_fs_srv_event_alloc( int manual, int signaled )
{
    if (ios_fs_config()->mode == IOS_FS_OFF) return -1;
    return ios_cell_alloc( IOS_CELL_EVENT, !!manual, 0, signaled ? IOS_CELL_SET : IOS_CELL_RESET );
}

int ios_fs_srv_sem_alloc( unsigned int initial, unsigned int max )
{
    const struct ios_fs_config *config = ios_fs_config();

    if (config->mode == IOS_FS_OFF || !config->sem) return -1;
    if (max < 1 || max > 0x7fffffff || initial > max) return -1;
    return ios_cell_alloc( IOS_CELL_SEMAPHORE, 0, max, (int32_t)initial );
}

void ios_fs_srv_free( int cell )
{
    ios_cell_free( cell );
}

/* B6: incremented before the entry is queued (and the object evaluated), decremented after */
void ios_fs_srv_queue( int cell, int delta )
{
    __atomic_add_fetch( &ios_cell_at( cell )->srv_waiters, delta, __ATOMIC_SEQ_CST );
}

/* ---- C3: events ---- */

int ios_fs_ev_state( int cell )
{
    int32_t state = ios_cell_state( ios_cell_load( ios_cell_at( cell )));

    if (state == IOS_CELL_DISABLED) return -1;
    return state == IOS_CELL_SET || state == IOS_CELL_CLAIMED;
}

int ios_fs_ev_signaled( int cell )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );

    for (;;)
    {
        int32_t state = ios_cell_state( word );

        if (state == IOS_CELL_DISABLED) return -1;
        if (cell_manual( c )) return state != IOS_CELL_RESET;
        if (state == IOS_CELL_CLAIMED) return 1;   /* this pass's own claim */
        if (state != IOS_CELL_SET) return 0;
        /* claim the token: no client can take it between signaled and satisfied */
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), IOS_CELL_CLAIMED ))) return 1;
        /* a client took it (or took it and set it again): look again */
    }
}

int ios_fs_ev_satisfied( int cell )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );

    for (;;)
    {
        int32_t state = ios_cell_state( word );

        if (state == IOS_CELL_DISABLED) return -1;
        if (cell_manual( c ) || state == IOS_CELL_RESET) return 0;
        /* consumes the claim, and a set that landed on top of it with it */
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), IOS_CELL_RESET ))) return 0;
    }
}

int ios_fs_ev_signal( int cell, int set, int *prev )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    int32_t target = set ? IOS_CELL_SET : IOS_CELL_RESET, state;
    uint64_t word = ios_cell_load( c );

    for (;;)
    {
        state = ios_cell_state( word );
        if (state == IOS_CELL_DISABLED) return -1;   /* a pulse got there first */
        if (state == target) break;
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), target ))) break;
    }
    if (prev) *prev = state == IOS_CELL_SET || state == IOS_CELL_CLAIMED;
    if (set) wake_clients( c, cell_manual( c ));
    return 0;
}

int ios_fs_ev_disable( int cell )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );
    int32_t state;

    for (;;)
    {
        state = ios_cell_state( word );
        if (state == IOS_CELL_DISABLED) return -1;
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), IOS_CELL_DISABLED ))) break;
    }
    wake_clients( c, 1 );
    return state == IOS_CELL_SET || state == IOS_CELL_CLAIMED;
}

void ios_fs_ev_unclaim( int cell )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );

    for (;;)
    {
        if (ios_cell_state( word ) != IOS_CELL_CLAIMED) return;
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), IOS_CELL_SET ))) break;
    }
    wake_clients( c, 0 );
}

/* ---- C4: semaphores ---- */

int ios_fs_sem_release( int cell, unsigned int count, unsigned int *prev )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint32_t max = __atomic_load_n( &c->max, __ATOMIC_RELAXED );
    uint64_t word = ios_cell_load( c );
    int32_t state;

    for (;;)
    {
        state = ios_cell_state( word );
        if (state < 0) return IOS_FS_SEM_DISABLED;
        if (count > max || (uint64_t)state + count > max)
        {
            *prev = state;
            return IOS_FS_SEM_LIMIT;
        }
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), state + (int32_t)count ))) break;
    }
    *prev = state;
    /* there is no "wake exactly n": a woken client that loses parks again */
    if (count) wake_clients( c, count > 1 );
    return IOS_FS_SEM_OK;
}

int ios_fs_sem_take( int cell )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );

    for (;;)
    {
        int32_t state = ios_cell_state( word );

        if (state < 0) return -1;
        if (!state) return 0;
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), state - 1 ))) return 1;
    }
}

int ios_fs_sem_give_back( int cell )
{
    static int over_logged;
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint32_t max = __atomic_load_n( &c->max, __ATOMIC_RELAXED );
    uint64_t word = ios_cell_load( c );

    for (;;)
    {
        int32_t state = ios_cell_state( word );

        if (state < 0) return -1;
        if ((uint32_t)state >= max)
        {
            /* a client released into the room the claimed token left: the semaphore is full,
             * the token goes (QUESTIONS.md, spec 16) */
            if (!over_logged)
            {
                over_logged = 1;
                ios_sync_log( "fastsync: semaphore cell %d full when a claimed token came back; dropped", cell );
            }
            return 0;
        }
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), state + 1 ))) break;
    }
    wake_clients( c, 0 );
    return 0;
}

int ios_fs_sem_disable( int cell )
{
    struct ios_sync_cell *c = ios_cell_at( cell );
    uint64_t word = ios_cell_load( c );
    int32_t state;

    for (;;)
    {
        state = ios_cell_state( word );
        if (state < 0) return -1;
        if (ios_cell_cas( c, &word, ios_cell_make( ios_cell_gen( word ), IOS_CELL_DISABLED ))) break;
    }
    wake_clients( c, 1 );
    return state;
}

int ios_fs_sem_count( int cell )
{
    int32_t state = ios_cell_state( ios_cell_load( ios_cell_at( cell )));
    return state < 0 ? -1 : state;
}
