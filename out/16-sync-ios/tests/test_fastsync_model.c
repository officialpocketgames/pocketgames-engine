// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part J2: Parts B-C as a model. A fake server runs the hooks the way spec 08's event.c,
// semaphore.c and thread.c do (one at a time: every server step holds one lock), against client
// threads doing Part D's operations. Checked: an auto-event set releases at most one waiter and
// no token is lost; a semaphore never passes max nor goes below 0 and every token is accounted
// for; a failed wait-all leaves every object as it was; a stale (index, generation) never takes
// a token from a recycled cell; disable folds the state; the timer re-check.

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ios_sync.h"
#include "check.h"

/* ======== the fake server ======== */

#define MAX_QUEUE 64

struct swait;

struct sobj
{
    int           is_sem;
    int           cell;
    /* the server's own fields (the fallback) */
    int           manual, signaled;
    unsigned int  count, max, claims;
    struct swait *queue[MAX_QUEUE];
    int           nqueue;
};

struct swait
{
    struct sobj   *objs[4];
    int            n, all;
    int            done, status;
    pthread_cond_t cond;
};

static pthread_mutex_t server = PTHREAD_MUTEX_INITIALIZER;   /* "the server thread" */

static void lock_server(void)   { pthread_mutex_lock( &server ); }
static void unlock_server(void) { pthread_mutex_unlock( &server ); }

static void sobj_init_event( struct sobj *o, int manual, int signaled )
{
    memset( o, 0, sizeof(*o) );
    o->manual = manual;
    o->signaled = signaled;
    o->cell = ios_fs_srv_event_alloc( manual, signaled );
}

static void sobj_init_sem( struct sobj *o, unsigned int initial, unsigned int max )
{
    memset( o, 0, sizeof(*o) );
    o->is_sem = 1;
    o->count = initial;
    o->max = max;
    o->cell = ios_fs_srv_sem_alloc( initial, max );
}

static uint32_t sobj_gen( const struct sobj *o )
{
    return ios_cell_gen( ios_cell_load( ios_cell_at( o->cell )));
}

/* event.c / semaphore.c hooks */
static int s_signaled( struct sobj *o )
{
    int r;
    if (!o->is_sem)
    {
        if (o->cell >= 0 && (r = ios_fs_ev_signaled( o->cell )) >= 0) return r;
        return o->signaled;
    }
    if (o->cell >= 0 && (r = ios_fs_sem_take( o->cell )) >= 0)
    {
        if (r) o->claims++;
        return r;
    }
    return o->count > 0;
}

static void s_satisfied( struct sobj *o )
{
    if (!o->is_sem)
    {
        if (o->cell >= 0 && !ios_fs_ev_satisfied( o->cell )) { if (!o->manual) o->signaled = 0; return; }
        if (!o->manual) o->signaled = 0;
        return;
    }
    if (o->claims) { o->claims--; return; }
    if (o->cell >= 0 && ios_fs_sem_take( o->cell ) > 0) return;
    o->count--;
}

static void s_unclaim( struct sobj *o )
{
    if (!o->is_sem)
    {
        if (o->cell >= 0) ios_fs_ev_unclaim( o->cell );
        return;
    }
    if (!o->claims) return;
    o->claims--;
    if (o->cell < 0 || ios_fs_sem_give_back( o->cell ) < 0) o->count++;
}

static void s_add_queue( struct sobj *o, struct swait *w )
{
    if (o->cell >= 0) ios_fs_srv_queue( o->cell, 1 );   /* before the object is evaluated */
    o->queue[o->nqueue++] = w;
}

static void s_remove_queue( struct sobj *o, struct swait *w )
{
    int i, cell = o->cell;
    for (i = 0; i < o->nqueue; i++)
        if (o->queue[i] == w)
        {
            memmove( &o->queue[i], &o->queue[i + 1], (o->nqueue - i - 1) * sizeof(o->queue[0]) );
            o->nqueue--;
            break;
        }
    if (cell >= 0) ios_fs_srv_queue( cell, -1 );
}

/* thread.c's check_wait: -1 not satisfied, else the index (0 for wait-all) */
static int check_wait( struct swait *w )
{
    int i, not_ok = 0;
    if (w->all)
    {
        for (i = 0; i < w->n; i++) not_ok |= !s_signaled( w->objs[i] );
        if (!not_ok) return 0;
        for (i = 0; i < w->n; i++) s_unclaim( w->objs[i] );   /* C5 */
        return -1;
    }
    for (i = 0; i < w->n; i++) if (s_signaled( w->objs[i] )) return i;
    return -1;
}

static void end_wait( struct swait *w, int status )
{
    int i;
    if (status >= 0 && status < w->n)
    {
        if (w->all) for (i = 0; i < w->n; i++) s_satisfied( w->objs[i] );
        else s_satisfied( w->objs[status] );
    }
    for (i = 0; i < w->n; i++) s_remove_queue( w->objs[i], w );
    w->done = 1;
    w->status = status;
    pthread_cond_signal( &w->cond );
}

static void wake_up( struct sobj *o, int max )
{
    int i = 0;
    while (i < o->nqueue)
    {
        struct swait *w = o->queue[i];
        int r = check_wait( w );
        if (r < 0) { i++; continue; }
        end_wait( w, r );
        if (max && !--max) break;
        i = 0;
    }
}

/* requests */
static int server_set( struct sobj *o, int set, int *prev )   /* SET / RESET */
{
    if (o->cell >= 0 && !ios_fs_ev_signal( o->cell, set, prev ))
    {
        o->signaled = set;
        if (set) wake_up( o, !o->manual );
        return 0;
    }
    *prev = o->signaled;
    o->signaled = set;
    if (set) wake_up( o, !o->manual );
    return 0;
}

static void server_wake( struct sobj *o )   /* event_op WAKE */
{
    wake_up( o, !o->manual );
}

static void server_pulse( struct sobj *o )
{
    int prev;
    if (o->cell >= 0 && (prev = ios_fs_ev_disable( o->cell )) >= 0) o->signaled = prev;
    o->signaled = 1;
    wake_up( o, !o->manual );
    o->signaled = 0;
}

static int server_release( struct sobj *o, unsigned int n, unsigned int *prev )
{
    int r = o->cell >= 0 ? ios_fs_sem_release( o->cell, n, prev ) : IOS_FS_SEM_DISABLED;
    if (r == IOS_FS_SEM_LIMIT) return -1;
    if (r == IOS_FS_SEM_DISABLED)
    {
        *prev = o->count;
        if (o->count + n > o->max) return -1;
        o->count += n;
    }
    wake_up( o, 0 );   /* no limit, always */
    return 0;
}

/* a server wait; timeout_ms < 0: infinite. Returns the index, or ETIMEDOUT. The timer handler
 * re-checks the objects first (C6). deliver_timer = 0 leaves the timer to the test. */
static int server_wait( struct sobj **objs, int n, int all, int timeout_ms )
{
    struct swait w;
    struct timespec ts;
    int i, r;

    memset( &w, 0, sizeof(w) );
    pthread_cond_init( &w.cond, NULL );
    w.n = n;
    w.all = all;
    for (i = 0; i < n; i++) w.objs[i] = objs[i];

    lock_server();
    for (i = 0; i < n; i++) s_add_queue( objs[i], &w );
    if ((r = check_wait( &w )) >= 0) end_wait( &w, r );
    else if (!timeout_ms) end_wait( &w, ETIMEDOUT );
    clock_gettime( CLOCK_REALTIME, &ts );
    ts.tv_nsec += (long)(timeout_ms > 0 ? timeout_ms : 0) * 1000000;
    ts.tv_sec += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
    while (!w.done)
    {
        if (timeout_ms < 0) pthread_cond_wait( &w.cond, &server );
        else if (pthread_cond_timedwait( &w.cond, &server, &ts ) == ETIMEDOUT && !w.done)
        {
            /* the timer: re-check before reporting a timeout */
            r = check_wait( &w );
            end_wait( &w, r >= 0 ? r : ETIMEDOUT );
        }
    }
    unlock_server();
    pthread_cond_destroy( &w.cond );
    return w.status;
}

/* ======== the client side (Part D) ======== */

/* NtSetEvent */
static int client_set( struct sobj *o, uint32_t gen, int set, int send_wake )
{
    int prev, wake, r = ios_fs_cli_event_change( o->cell, gen, set, &prev, &wake );
    if (r == IOS_FS_GO_SERVER)
    {
        lock_server();
        server_set( o, set, &prev );
        unlock_server();
    }
    else if (wake && send_wake)
    {
        lock_server();
        server_wake( o );
        unlock_server();
    }
    return prev;
}

/* NtReleaseSemaphore: 0 ok, -1 limit */
static int client_release( struct sobj *o, uint32_t gen, unsigned int n )
{
    unsigned int prev;
    int wake, r = ios_fs_cli_sem_release( o->cell, gen, n, &prev, &wake );

    if (r == 1) return -1;
    if (r == IOS_FS_GO_SERVER)
    {
        lock_server();
        r = server_release( o, n, &prev );
        unlock_server();
        return r;
    }
    if (wake)
    {
        lock_server();
        server_release( o, 0, &prev );   /* "re-run your queue" */
        unlock_server();
    }
    return 0;
}

/* NtWaitForSingleObject: the fast wait, then the server with what is left; 1 when satisfied */
static int client_wait( struct sobj *o, uint32_t gen, int timeout_ms, int fast )
{
    uint64_t spent = 0, cap = 0;
    int left = timeout_ms;

    if (fast)
    {
        if (ios_fs_cli_wait( o->cell, gen, (uint64_t)timeout_ms * 1000000, &spent, &cap )) return 1;
        left = (int)(ios_fs_remaining_100ns( (int64_t)timeout_ms * 10000, spent, cap ) / 10000);
    }
    return server_wait( &o, 1, 0, left ) == 0;
}

/* ======== the concurrent runs ======== */

static struct sobj ev;
static uint32_t ev_gen;
static int stop_flag;
#define stop          __atomic_load_n( &stop_flag, __ATOMIC_ACQUIRE )
#define set_stop( v ) __atomic_store_n( &stop_flag, (v), __ATOMIC_RELEASE )
static long released, transitions;

static void *ev_waiter( void *arg )
{
    int fast = (int)(intptr_t)arg;
    while (!stop)
        if (client_wait( &ev, ev_gen, 20, fast )) __atomic_add_fetch( &released, 1, __ATOMIC_RELAXED );
    return NULL;
}

static void *ev_setter( void *arg )
{
    int i;
    (void)arg;
    for (i = 0; i < 3000; i++)
    {
        if (!client_set( &ev, ev_gen, 1, 1 )) __atomic_add_fetch( &transitions, 1, __ATOMIC_RELAXED );
        if (i % 50 == 0) sched_yield();
    }
    return NULL;
}

static void test_auto_event_concurrent(void)
{
    pthread_t waiters[6], setters[3];
    int i, final;

    sobj_init_event( &ev, 0, 0 );
    CHECK( ev.cell >= 0 );
    ev_gen = sobj_gen( &ev );
    /* fast waiters and server-only waiters (claims, srv_waiters, WAKE requests) */
    for (i = 0; i < 6; i++) pthread_create( &waiters[i], NULL, ev_waiter, (void *)(intptr_t)(i % 2) );
    for (i = 0; i < 3; i++) pthread_create( &setters[i], NULL, ev_setter, NULL );
    for (i = 0; i < 3; i++) pthread_join( setters[i], NULL );
    /* let the waiters take what is left */
    for (i = 0; i < 100 && ios_fs_ev_state( ev.cell ); i++)
    {
        struct timespec ts = { 0, 10000000 };
        nanosleep( &ts, NULL );
    }
    set_stop( 1 );
    for (i = 0; i < 6; i++) pthread_join( waiters[i], NULL );
    final = ios_fs_ev_state( ev.cell );
    /* one set (a RESET -> SET transition), one release; nothing lost */
    CHECK_EQ( released + final, transitions );
    CHECK( transitions > 100 );
    CHECK_EQ( ios_cell_srv_waiters( ios_cell_at( ev.cell )), 0 );
    ios_fs_srv_free( ev.cell );
    set_stop( 0 );
}

static struct sobj sem;
static uint32_t sem_gen;
static long sem_released, sem_consumed, sem_bad;

static void *sem_waiter( void *arg )
{
    int fast = (int)(intptr_t)arg;
    while (!stop)
        if (client_wait( &sem, sem_gen, 20, fast )) __atomic_add_fetch( &sem_consumed, 1, __ATOMIC_RELAXED );
    return NULL;
}

static void *sem_releaser( void *arg )
{
    unsigned int i, n;
    (void)arg;
    for (i = 0; i < 4000; i++)
    {
        n = 1 + i % 3;
        if (!client_release( &sem, sem_gen, n )) __atomic_add_fetch( &sem_released, n, __ATOMIC_RELAXED );
        if (i % 20 == 0) sched_yield();
    }
    return NULL;
}

static void *sem_monitor( void *arg )
{
    (void)arg;
    while (!stop)
    {
        int32_t st = ios_cell_state( ios_cell_load( ios_cell_at( sem.cell )));
        if (st < 0 || st > (int32_t)sem.max) __atomic_add_fetch( &sem_bad, 1, __ATOMIC_RELAXED );
    }
    return NULL;
}

static void test_semaphore_concurrent(void)
{
    pthread_t waiters[6], releasers[2], monitor;
    int i;

    sobj_init_sem( &sem, 0, 50 );
    CHECK( sem.cell >= 0 );
    sem_gen = sobj_gen( &sem );
    pthread_create( &monitor, NULL, sem_monitor, NULL );
    for (i = 0; i < 6; i++) pthread_create( &waiters[i], NULL, sem_waiter, (void *)(intptr_t)(i % 2) );
    for (i = 0; i < 2; i++) pthread_create( &releasers[i], NULL, sem_releaser, NULL );
    for (i = 0; i < 2; i++) pthread_join( releasers[i], NULL );
    for (i = 0; i < 100 && ios_fs_sem_count( sem.cell ); i++)
    {
        struct timespec ts = { 0, 10000000 };
        nanosleep( &ts, NULL );
    }
    set_stop( 1 );
    for (i = 0; i < 6; i++) pthread_join( waiters[i], NULL );
    pthread_join( monitor, NULL );
    CHECK_EQ( sem_bad, 0 );
    CHECK_EQ( sem.claims, 0 );
    CHECK_EQ( sem_consumed + ios_fs_sem_count( sem.cell ), sem_released );
    CHECK( sem_released > 1000 );
    ios_fs_srv_free( sem.cell );
    set_stop( 0 );
}

/* ======== deterministic cases ======== */

static void test_wait_all_fails_cleanly(void)
{
    struct sobj e, s, m, *objs[3];
    struct swait w;

    sobj_init_event( &e, 0, 1 );   /* auto, SET */
    sobj_init_event( &m, 1, 1 );   /* manual, SET */
    sobj_init_sem( &s, 2, 5 );
    struct sobj z;
    sobj_init_sem( &z, 0, 5 );     /* empty: the wait-all can't complete */

    objs[0] = &e; objs[1] = &s; objs[2] = &z;
    memset( &w, 0, sizeof(w) );
    w.n = 3; w.all = 1;
    w.objs[0] = &e; w.objs[1] = &s; w.objs[2] = &z;
    lock_server();
    CHECK_EQ( check_wait( &w ), -1 );
    unlock_server();
    /* every object as it was */
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( e.cell ))), IOS_CELL_SET );
    CHECK_EQ( ios_fs_sem_count( s.cell ), 2 );
    CHECK_EQ( s.claims, 0 );
    CHECK_EQ( ios_fs_sem_count( z.cell ), 0 );
    /* a server wait-all with a timeout leaves them as well */
    CHECK_EQ( server_wait( objs, 3, 1, 5 ), ETIMEDOUT );
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( e.cell ))), IOS_CELL_SET );
    CHECK_EQ( ios_fs_sem_count( s.cell ), 2 );
    /* with the last object there, it takes all of them */
    lock_server();
    { unsigned int prev; server_release( &z, 1, &prev ); }
    unlock_server();
    CHECK_EQ( server_wait( objs, 3, 1, 0 ), 0 );
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( e.cell ))), IOS_CELL_RESET );
    CHECK_EQ( ios_fs_sem_count( s.cell ), 1 );
    CHECK_EQ( ios_fs_sem_count( z.cell ), 0 );
    /* a manual event is never claimed */
    objs[0] = &m;
    CHECK_EQ( server_wait( objs, 3, 1, 0 ), ETIMEDOUT );
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( m.cell ))), IOS_CELL_SET );
    ios_fs_srv_free( e.cell );
    ios_fs_srv_free( m.cell );
    ios_fs_srv_free( s.cell );
    ios_fs_srv_free( z.cell );
}

static void test_stale_generation(void)
{
    int cell = ios_cell_alloc( IOS_CELL_EVENT, 0, 0, IOS_CELL_SET ), again;
    uint32_t gen = ios_cell_gen( ios_cell_load( ios_cell_at( cell ))), gen2, prev;
    int p, wake;

    ios_cell_free( cell );
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( cell ))), IOS_CELL_DISABLED );
    again = ios_cell_alloc( IOS_CELL_SEMAPHORE, 0, 3, 3 );
    CHECK_EQ( again, cell );   /* last freed first */
    gen2 = ios_cell_gen( ios_cell_load( ios_cell_at( cell )));
    CHECK( gen2 != gen );
    CHECK( gen2 != 0 );
    /* the old pair takes nothing and changes nothing */
    CHECK_EQ( ios_fs_cli_try( cell, gen ), IOS_FS_GO_SERVER );
    CHECK_EQ( ios_fs_cli_event_change( cell, gen, 0, &p, &wake ), IOS_FS_GO_SERVER );
    CHECK_EQ( ios_fs_cli_sem_release( cell, gen, 0, &prev, &wake ), IOS_FS_GO_SERVER );
    CHECK( !ios_fs_cli_peek_unsignaled( cell, gen ));
    CHECK( !ios_fs_cli_reads_signaled( cell, gen ));
    CHECK_EQ( ios_fs_sem_count( cell ), 3 );
    {
        uint64_t spent, cap;
        CHECK_EQ( ios_fs_cli_wait( cell, gen, UINT64_MAX, &spent, &cap ), 0 );
    }
    CHECK_EQ( ios_fs_sem_count( cell ), 3 );
    CHECK_EQ( ios_fs_cli_try( cell, gen2 ), 1 );
    ios_cell_free( cell );
}

static void test_disable_folds(void)
{
    struct sobj e;
    int cell;

    /* events: SET, CLAIMED -> signalled; RESET -> not */
    cell = ios_cell_alloc( IOS_CELL_EVENT, 0, 0, IOS_CELL_SET );
    CHECK_EQ( ios_fs_ev_disable( cell ), 1 );
    CHECK_EQ( ios_fs_ev_disable( cell ), -1 );   /* one way */
    CHECK_EQ( ios_fs_ev_signaled( cell ), -1 );
    CHECK_EQ( ios_fs_ev_signal( cell, 1, NULL ), -1 );
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( cell ))), IOS_CELL_DISABLED );
    ios_cell_free( cell );
    cell = ios_cell_alloc( IOS_CELL_EVENT, 0, 0, IOS_CELL_SET );
    CHECK_EQ( ios_fs_ev_signaled( cell ), 1 );   /* claims */
    CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( cell ))), IOS_CELL_CLAIMED );
    CHECK_EQ( ios_fs_ev_disable( cell ), 1 );
    ios_cell_free( cell );
    cell = ios_cell_alloc( IOS_CELL_EVENT, 1, 0, IOS_CELL_RESET );
    CHECK_EQ( ios_fs_ev_disable( cell ), 0 );
    ios_cell_free( cell );
    /* semaphores: the count */
    cell = ios_cell_alloc( IOS_CELL_SEMAPHORE, 0, 9, 4 );
    CHECK_EQ( ios_fs_sem_disable( cell ), 4 );
    CHECK_EQ( ios_fs_sem_disable( cell ), -1 );
    CHECK_EQ( ios_fs_sem_count( cell ), -1 );
    CHECK_EQ( ios_fs_sem_take( cell ), -1 );
    { unsigned int prev; CHECK_EQ( ios_fs_sem_release( cell, 1, &prev ), IOS_FS_SEM_DISABLED ); }
    ios_cell_free( cell );

    /* a pulse takes the event out of the fast path and works on the field */
    sobj_init_event( &e, 0, 1 );
    lock_server();
    server_pulse( &e );
    unlock_server();
    CHECK_EQ( e.signaled, 0 );
    CHECK_EQ( ios_fs_ev_state( e.cell ), -1 );
    {
        int prev, wake;
        CHECK_EQ( ios_fs_cli_event_change( e.cell, sobj_gen( &e ), 1, &prev, &wake ), IOS_FS_GO_SERVER );
        CHECK_EQ( ios_fs_cli_try( e.cell, sobj_gen( &e )), IOS_FS_GO_SERVER );
    }
    ios_fs_srv_free( e.cell );
}

/* C6: a client raised the object without a request; the timer finds it */
static void test_timer_recheck(void)
{
    struct sobj e, *objs[1];
    int r;

    sobj_init_event( &e, 0, 0 );
    objs[0] = &e;
    /* a client sets the cell between the server's queueing and the timer, and its WAKE request
     * is never delivered (still in the socket): only the timer's re-check can see it */
    {
        struct swait w;
        memset( &w, 0, sizeof(w) );
        pthread_cond_init( &w.cond, NULL );
        w.n = 1; w.objs[0] = &e;
        lock_server();
        s_add_queue( &e, &w );
        CHECK_EQ( check_wait( &w ), -1 );
        unlock_server();
        client_set( &e, sobj_gen( &e ), 1, 0 );   /* no WAKE */
        lock_server();
        r = check_wait( &w );                       /* what the timer handler does first */
        end_wait( &w, r >= 0 ? r : ETIMEDOUT );
        unlock_server();
        CHECK_EQ( w.status, 0 );
        CHECK_EQ( ios_cell_state( ios_cell_load( ios_cell_at( e.cell ))), IOS_CELL_RESET );
        pthread_cond_destroy( &w.cond );
    }
    /* and with nothing there, a timeout */
    CHECK_EQ( server_wait( objs, 1, 0, 5 ), ETIMEDOUT );
    ios_fs_srv_free( e.cell );
}

/* C1: a full table is a tuning limit, the object stays on the server */
static void test_table_full(void)
{
    static int cells[IOS_SYNC_CELLS];
    int i, n = 0, extra;

    for (i = 0; i < IOS_SYNC_CELLS; i++)
        if ((cells[i] = ios_cell_alloc( IOS_CELL_EVENT, 0, 0, 0 )) >= 0) n++;
        else break;
    extra = ios_fs_srv_event_alloc( 0, 0 );
    CHECK_EQ( extra, -1 );
    for (i = 0; i < n; i++) ios_cell_free( cells[i] );
    CHECK( ios_cells_in_use() );
}

int main(void)
{
    setenv( "MADEIRA_FASTSYNC", "on", 1 );
    setenv( "MADEIRA_FASTSYNC_SEM", "1", 1 );
    setenv( "MADEIRA_FASTSYNC_CAP_US", "500", 1 );

    test_stale_generation();
    test_disable_folds();
    test_wait_all_fails_cleanly();
    test_timer_recheck();
    test_auto_event_concurrent();
    test_semaphore_concurrent();
    test_table_full();
    return check_done( "test_fastsync_model" );
}
