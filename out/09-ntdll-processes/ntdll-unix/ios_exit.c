// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part D: ending pseudo-processes, and the thread alert of Part D3.
//
// Every Windows thread leaves through ios_thread_leave() (thread.c), called by upstream's
// abort_thread() and exit_thread(). The last thread of a pseudo-process finishes it
// (finish_process below). NtTerminateProcess on the own process (exit_process/abort_process)
// comes here through ios_end_pseudo_process(): it alerts the other threads, waits for them to
// leave, and then leaves itself, last.
//
// The thread that called __wine_main for the session's first program never pthread_exits: it
// waits (parked) until its program has ended and then longjmps back to the app (Part C2).

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"
#include "ios_pure.h"
#include "ios_sig_pure.h"       /* spec 22 C5: the thread registry (Mach port -> TEB) */
#include "ios_session_pure.h"   /* spec 22 */

#ifdef WINE_IOS

/* how long the ending thread waits for the others, and how often it alerts them again */
#define IOS_END_WAIT_MS     2000
#define IOS_END_REALERT_MS  100
/* spec 22 C5: how long threads an ended process left get after their alert */
#define IOS_LEFTOVER_WAIT_MS 500

/* ---------- Part D3: who is alertable ----------
 *
 * The server knows a thread by its unix tid (its Mach thread port name, spec 08 Part C4); the
 * in-process waits are keyed by the Windows thread id (sync.c). This table maps one to the
 * other. Lock-free: the server thread calls ios_thread_alert() and must never block on a lock a
 * Windows thread holds. A slot is claimed with a compare-and-swap on unix_tid and published by
 * the release store of tid. */

#define IOS_ALERT_SLOTS 1024

struct alert_slot
{
    unsigned int    unix_tid;   /* 0: free */
    DWORD           tid;        /* 0: being written */
    struct ios_proc *proc;
};

static struct alert_slot alert_slots[IOS_ALERT_SLOTS];

/* the hook spec 08's server calls for every thread it kills or must wake (spec 08
 * clarification 4, spec 11 clarification 2) */
extern void (*wineserver_thread_alert_hook)( unsigned int unix_tid );


/***********************************************************************
 *           ios_thread_started
 *
 * A Windows thread of the calling pseudo-process now exists on the server (tid) and as a Mach
 * thread (unix_tid).
 */
void ios_thread_started( DWORD tid, unsigned int unix_tid )
{
    struct ios_proc *proc = ios_proc_current();
    unsigned int i, expected;

    if (!unix_tid || !tid) return;
    /* a stale entry for a reused port name (a thread that died without leaving) */
    for (i = 0; i < IOS_ALERT_SLOTS; i++)
    {
        if (__atomic_load_n( &alert_slots[i].unix_tid, __ATOMIC_ACQUIRE ) != unix_tid) continue;
        alert_slots[i].proc = proc;
        __atomic_store_n( &alert_slots[i].tid, tid, __ATOMIC_RELEASE );
        return;
    }
    for (i = 0; i < IOS_ALERT_SLOTS; i++)
    {
        expected = 0;
        if (!__atomic_compare_exchange_n( &alert_slots[i].unix_tid, &expected, unix_tid, FALSE,
                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED )) continue;
        alert_slots[i].proc = proc;
        __atomic_store_n( &alert_slots[i].tid, tid, __ATOMIC_RELEASE );
        return;
    }
    ios_log( "alert table full: thread %04x can't be alerted\n", (int)tid );
}


/* the calling thread is leaving: it can't be alerted any more */
static void thread_forget( DWORD tid )
{
    unsigned int i;

    for (i = 0; i < IOS_ALERT_SLOTS; i++)
    {
        if (__atomic_load_n( &alert_slots[i].tid, __ATOMIC_ACQUIRE ) != tid) continue;
        if (!__atomic_load_n( &alert_slots[i].unix_tid, __ATOMIC_ACQUIRE )) continue;
        __atomic_store_n( &alert_slots[i].tid, 0, __ATOMIC_RELEASE );
        alert_slots[i].proc = NULL;
        __atomic_store_n( &alert_slots[i].unix_tid, 0, __ATOMIC_RELEASE );
    }
}


/***********************************************************************
 *           ios_thread_alert
 *
 * Part D3 (used by spec 08): wakes the thread with this unix tid out of any in-process wait
 * (futex, NtDelayExecution); the woken thread then asks the server, which makes a killed
 * thread leave and runs a pending system APC. Does nothing for a thread that has gone.
 * Safe on the server thread: no locks, no allocation.
 */
void ios_thread_alert( unsigned int unix_tid )
{
    unsigned int i;
    DWORD tid;

    if (!unix_tid) return;
    for (i = 0; i < IOS_ALERT_SLOTS; i++)
    {
        if (__atomic_load_n( &alert_slots[i].unix_tid, __ATOMIC_ACQUIRE ) != unix_tid) continue;
        if ((tid = __atomic_load_n( &alert_slots[i].tid, __ATOMIC_ACQUIRE ))) ios_alert_wake( tid );
        return;
    }
}


/* alert every thread of proc but the caller (an ending process's own threads) */
static void alert_process_threads( struct ios_proc *proc, DWORD self )
{
    unsigned int i;
    DWORD tid;

    for (i = 0; i < IOS_ALERT_SLOTS; i++)
    {
        if (!__atomic_load_n( &alert_slots[i].unix_tid, __ATOMIC_ACQUIRE )) continue;
        if (alert_slots[i].proc != proc) continue;
        if (!(tid = __atomic_load_n( &alert_slots[i].tid, __ATOMIC_ACQUIRE )) || tid == self) continue;
        ios_alert_wake( tid );
    }
}


/***********************************************************************
 *           ios_alert_own_process
 *
 * NtTerminateProcess on the own process: the other threads must notice now (Part D1).
 */
void ios_alert_own_process(void)
{
    alert_process_threads( ios_proc_current(), GetCurrentThreadId() );
}


/***********************************************************************
 *           ios_handle_poke
 *
 * Called by a thread woken by ios_thread_alert. Inside an uninterruptible section it waits
 * until the outermost section is left (server.c); otherwise it asks the server now. A killed
 * thread's pipes are closed by the server, so the request fails and the thread leaves through
 * abort_thread(); a pending system APC runs; anything else returns at once (timeout 0).
 */
void ios_handle_poke(void)
{
    struct ntdll_thread_data *data;

    if (!ios_current_teb()) return;
    data = ntdll_get_thread_data();
    if (data->ios_leaving) return;
    if (data->ios_no_interrupt)
    {
        data->ios_poke_pending = 1;
        return;
    }
    data->ios_poke_pending = 0;
    server_select( NULL, 0, SELECT_INTERRUPTIBLE, 0, NULL, NULL );
}


/* ---------- spec 22 C3: a thread the server ended ---------- */

static _Thread_local int thread_killed;

/***********************************************************************
 *           ios_thread_killed
 *
 * server.c: the server closed the calling thread's pipes (it was killed, alone or with its
 * process). From here on its requests fail instead of ending it half-way (server_call_unlocked).
 */
void ios_thread_killed( int status )
{
    thread_killed = 1;
    abort_thread( status );
}

BOOL ios_thread_was_killed(void)
{
    return thread_killed;
}

static BOOL is_main_thread( struct ios_proc *proc )
{
    return __atomic_load_n( &proc->have_main, __ATOMIC_ACQUIRE ) && pthread_equal( proc->main_thread, pthread_self() );
}


/* ---------- spec 22 C6: process-end hooks ---------- */

static struct ios_hook_list end_hooks;

/***********************************************************************
 *           ios_process_end_hook_add   (cross-library, spec 22 C6)
 *
 * fn runs with an ended pseudo-process's PEB after its threads are gone, before its memory is
 * given back. At most 16, each once. 0 when registered (or already), -1 otherwise.
 */
int ios_process_end_hook_add( void (*fn)( const void *peb ))
{
    int ret = ios_hook_list_add( &end_hooks, fn );

    if (ret < 0) ios_log( "process-end hook %p not added: %s\n", (void *)fn, fn ? "16 already" : "NULL" );
    return ret < 0 ? -1 : 0;
}

static void run_end_hooks( struct ios_proc *proc )
{
    int n;

    if (!proc->pp_peb) return;
    n = ios_hook_list_run( &end_hooks, proc->pp_peb );
    if (n && !ios_quiet) ios_log( "process %u (%s): %d process-end hook(s) ran\n", proc->serial, proc->name, n );
}


/* ---------- Part C2: back to the app ---------- */

/* __wine_main's own exit buffer on the thread that called it: the way back when the app's
 * buffer isn't set for this thread, so that thread is never ended by pthread_exit */
static _Thread_local jmp_buf *own_exit_buf;
static _Thread_local int own_exit_code;

void ios_set_own_exit_buf( jmp_buf *buf )
{
    own_exit_buf = buf;
}

int ios_own_exit_code(void)
{
    return own_exit_code;
}

static BOOL app_exit_ready(void)
{
    return wine_ios_exit_initialized == 1 && pthread_equal( pthread_self(), wine_ios_main_thread );
}

/* the thread that called __wine_main (Part C2) */
BOOL ios_is_session_thread(void)
{
    return app_exit_ready() || own_exit_buf != NULL;
}

static void __attribute__((noreturn)) session_return( int exit_code )
{
    jmp_buf *own = own_exit_buf;

    ios_set_thread_proc( NULL );
    own_exit_buf = NULL;
    if (wine_launched_process_did_exit) wine_launched_process_did_exit( exit_code );
    switch (ios_exit_route( app_exit_ready(), own != NULL ))
    {
    case IOS_EXIT_APP_JMP:
        ios_boot_mark( "the program ended (exit code %d): back to the app", exit_code );
        wine_ios_exit_code = exit_code;
        longjmp( wine_ios_exit_jmpbuf, 1 );
    case IOS_EXIT_OWN_JMP:
        ios_boot_mark( "the program ended (exit code %d): __wine_main returns (the app's exit "
                       "buffer isn't set for this thread)", exit_code );
        own_exit_code = exit_code;
        longjmp( *own, 1 );
    default:
        pthread_exit( NULL );
    }
}

/* a thread with no Windows state leaves: quietly (Part E3) */
static void __attribute__((noreturn)) native_thread_exit( int status )
{
    if (ios_is_session_thread()) session_return( status );
    pthread_exit( NULL );
}


/***********************************************************************
 *           ios_assert_fail
 *
 * Part A "no exits": a failed assert() of the unix side (assert.h of this folder) ends the
 * calling pseudo-process like abort() instead of aborting the app.
 */
void ios_assert_fail( const char *expr, const char *file, int line )
{
    ios_log( "assertion failed: %s (%s:%d)\n", expr, file, line );
    ios_unix_exit( IOS_ABORT_EXIT_CODE, "assert" );
}


/***********************************************************************
 *           ios_unix_exit
 *
 * Part A "no exits": exit(), _exit() and abort() of the unix side end the calling
 * pseudo-process, or only the thread when it has no TEB.
 */
void ios_unix_exit( int status, const char *why )
{
    struct ios_proc *proc = ios_proc_current();

    ios_log( "%s(%d) in process %u (%s): ending %s\n", why, status, proc->serial, proc->name,
             ios_current_teb() ? "its pseudo-process" : "the thread" );
    if (!ios_current_teb())
    {
        if (proc->state == IOS_PROC_BOOTING && proc->serial) ios_boot_failed( why, status );
        native_thread_exit( status );
    }
    ios_end_pseudo_process( status );
}


/***********************************************************************
 *           ios_boot_failed
 *
 * Part D2: a pseudo-process that can't boot (no server, no guest window, no process
 * parameters yet). Logs why and ends its boot thread; the session's first program returns to
 * the app instead (Part C2).
 */
void ios_boot_failed( const char *why, NTSTATUS status )
{
    struct ios_proc *proc = ios_proc_current();

    ios_log( "process %u (%s) can't boot: %s (stage: %s)\n", proc->serial, proc->name, why,
             ios_stage_name( proc->stage ));
    if (proc->boot_socket != -1) close( proc->boot_socket );
    proc->boot_socket = -1;
    if (proc->pp_fd_socket != -1) close( proc->pp_fd_socket );
    proc->pp_fd_socket = -1;
    ios_children_remove( proc->child_id );
    proc->child_id = 0;
    ios_proc_unregister( proc );
    InterlockedExchange( &proc->state, IOS_PROC_DEAD );
    if (ios_teb_key_ready && ios_current_teb()) pthread_setspecific( teb_key, NULL );
    native_thread_exit( (int)status );
}


/* ---------- Part D1: the end ---------- */

/* the parts of the end that need no thread state: done by the last thread to leave, or by the
 * ending thread when some threads didn't leave in time */
static void finish_process( struct ios_proc *proc, int exit_code )
{
    if (proc->stage < IOS_STAGE_RUNNING)
        ios_log( "process %u (%s) died during boot (stage: %s)\n", proc->serial, proc->name,
                 ios_stage_name( proc->stage ));

    /* spec 10 frees what must be free before the server sees the end (the exe window) */
    ios_mem_process_ending( proc );

    /* the server sees the process end by EOF on its socket */
    if (proc->pp_fd_socket != -1) close( proc->pp_fd_socket );
    proc->pp_fd_socket = -1;

    /* handles are gone with the process: drop the caches and close their fds */
    ios_fd_cache_drop( proc );
    ios_inproc_cache_drop( proc );
    if (proc->pp_inproc_device_fd != -1) close( proc->pp_inproc_device_fd );
    proc->pp_inproc_device_fd = -1;
    ios_dir_cache_drop( proc );

    ios_children_remove( proc->child_id );
    proc->child_id = 0;

    /* forget the slot: a later pseudo-process whose PEB lands here starts clean */
    ios_proc_unregister( proc );

    ios_log( "process %u (%s) ended, exit code %d (0x%x)\n", proc->serial, proc->name,
             exit_code, exit_code );
}


/* Marks the process dead after its last thread has left, and wakes the session's thread if it
 * waits for it. Returns TRUE when the caller frees the struct: otherwise the waiting session
 * thread does. The caller must not touch proc after a FALSE return. */
static BOOL mark_dead_last( struct ios_proc *proc )
{
    BOOL mine;

    pthread_mutex_lock( &proc->mutex );
    proc->state = IOS_PROC_DEAD;
    proc->all_left = TRUE;
    mine = !proc->session_parked;
    pthread_cond_broadcast( &proc->cond );
    pthread_mutex_unlock( &proc->mutex );
    return mine;
}

/* Marks the process dead while some of its threads haven't left (they are stuck). */
static void mark_dead_early( struct ios_proc *proc )
{
    pthread_mutex_lock( &proc->mutex );
    proc->state = IOS_PROC_DEAD;
    pthread_cond_broadcast( &proc->cond );
    pthread_mutex_unlock( &proc->mutex );
}


/***********************************************************************
 *           ios_thread_leave
 *
 * Part E5 and D: the one way a Windows thread leaves. Called by thread.c with the TEB still
 * set, after upstream's deferred release of the previous thread's TEB. Never returns.
 */
void ios_thread_leave( int status )
{
    struct ios_proc *proc;
    struct ntdll_thread_data *data;
    TEB *teb = ios_current_teb();
    BOOL session, mine;
    LONG left;
    int code;

    if (!teb) native_thread_exit( status );
    data = ntdll_get_thread_data();
    proc = ios_proc_current();
    session = ios_is_session_thread();

    if (data->ios_leaving)
    {
        /* a server call failed on the way out: just go */
        if (session) session_return( proc->exit_code );
        pthread_exit( NULL );
    }

    /* spec 22 C3: the server ended the process and woke its main thread: that thread ends the
     * process (the others are alerted, the end comes back to its launch thread), not only itself */
    if (ios_killed_thread_ends_process( thread_killed, is_main_thread( proc ),
                                        (int)__atomic_load_n( &proc->pp_nb_threads, __ATOMIC_ACQUIRE ) - 1,
                                        proc->state >= IOS_PROC_ENDING ))
    {
        ios_log( "process %u (%s): its main thread was ended by the server: the process ends\n",
                 proc->serial, proc->name );
        ios_end_pseudo_process( status );
    }
    data->ios_leaving = 1;

    /* Part D3: a thread dying inside a section that must not be interrupted leaves it */
    ios_release_held_locks( data );
    thread_forget( GetCurrentThreadId() );
    /* spec 11 Parts B and D: alternate signal stack off, out of the thread registry */
    ios_signal_thread_leave();

    left = InterlockedDecrement( &proc->pp_nb_threads );
    code = (proc->state >= IOS_PROC_ENDING) ? proc->exit_code : status;

    if (left <= 0 && proc->state != IOS_PROC_DEAD)
    {
        /* the last thread ends the process (as upstream's exit_thread -> exit_process) */
        if (proc->state < IOS_PROC_ENDING) proc->exit_code = code;
        InterlockedExchange( &proc->state, IOS_PROC_ENDING );
        finish_process( proc, proc->exit_code );
    }

    /* Part E5: the thread's own pipes, the memory layer, the TEB key */
    ios_close_thread_fds();
    ios_mem_thread_gone( teb );
    ios_set_thread_proc( proc );
    pthread_setspecific( teb_key, NULL );

    if (left <= 0)
    {
        /* nothing of this process runs any more: the other libraries' clean-up (spec 22 C6),
         * then its memory goes back (spec 10) */
        run_end_hooks( proc );
        ios_mem_release_process( proc, FALSE );
        code = proc->exit_code;
        mine = mark_dead_last( proc );   /* proc may be gone after this when !mine */
        ios_set_thread_proc( NULL );
        if (mine) ios_proc_free( proc );
        if (session) session_return( code );
        pthread_detach( pthread_self() );
        pthread_exit( NULL );
    }

    if (session)
    {
        /* Part C2: __wine_main returns only when the program has ended; wait for its end */
        pthread_mutex_lock( &proc->mutex );
        proc->session_parked = TRUE;
        while (proc->state != IOS_PROC_DEAD) pthread_cond_wait( &proc->cond, &proc->mutex );
        proc->session_parked = FALSE;
        code = proc->exit_code;
        mine = proc->all_left;
        pthread_mutex_unlock( &proc->mutex );
        ios_set_thread_proc( NULL );
        if (mine) ios_proc_free( proc );
        session_return( code );
    }

    ios_set_thread_proc( NULL );
    pthread_exit( NULL );
}


/* the TEB of the registered thread with this Mach port, or NULL */
static TEB *leftover_teb( unsigned int port )
{
    return (TEB *)ios_threadreg_teb_of( port );
}

/* the thread is inside one of Wine's locked sections, or half-way out */
static int leftover_in_wine( TEB *teb )
{
    const struct ntdll_thread_data *data;

    if (!teb) return 0;
    data = (const struct ntdll_thread_data *)&teb->GdiTebBatch;
    return data->ios_no_interrupt > 0 || data->ios_leaving;
}

/***********************************************************************
 *           forget_leftover_threads
 *
 * Spec 22 C5: the ending process still has threads after the normal wait. Alert them all; a woken
 * one finds its process ended at its next server request and leaves through the normal thread
 * exit. 0.5 s later, a thread still there that is blocked in the kernel outside any of Wine's
 * locks is suspended for good and forgotten (iOS can't end a thread from another thread): its
 * state is read before the suspension, which would report "stopped". A thread that runs (guest
 * code) is kept, and with it the process's memory.
 */
static void forget_leftover_threads( struct ios_proc *proc, DWORD self )
{
    struct ios_leftover_counts counts = { 0 };
    unsigned int i;
    int waited, remaining = 0, logged = 0;
    DWORD tid;

    for (i = 0; i < IOS_ALERT_SLOTS; i++)
    {
        if (!__atomic_load_n( &alert_slots[i].unix_tid, __ATOMIC_ACQUIRE ) || alert_slots[i].proc != proc) continue;
        if (!(tid = __atomic_load_n( &alert_slots[i].tid, __ATOMIC_ACQUIRE )) || tid == self) continue;
        ios_alert_wake( tid );
        counts.woken++;
    }
    for (waited = 0; waited < IOS_LEFTOVER_WAIT_MS &&
                     __atomic_load_n( &proc->pp_nb_threads, __ATOMIC_ACQUIRE ) > 1; waited += 10)
        usleep( 10000 );

    for (i = 0; i < IOS_ALERT_SLOTS; i++)
    {
        unsigned int port = __atomic_load_n( &alert_slots[i].unix_tid, __ATOMIC_ACQUIRE );
        enum ios_leftover_action action;
        int waiting;

        if (!port || alert_slots[i].proc != proc) continue;
        if (!(tid = __atomic_load_n( &alert_slots[i].tid, __ATOMIC_ACQUIRE )) || tid == self) continue;
        remaining++;
        waiting = ios_mach_thread_waiting( port );   /* before any suspension */
        action = ios_leftover_action( waiting < 0, waiting == 1, leftover_in_wine( leftover_teb( port )));
        if (action == IOS_LEFTOVER_SUSPEND)
        {
            if (ios_mach_thread_suspend( port ))
                action = IOS_LEFTOVER_RUNNING;
            else
            {
                thread_forget( tid );
                ios_threadreg_remove( port );
                InterlockedDecrement( &proc->pp_nb_threads );
                ios_log( "process %u (%s): thread %04x was blocked in a system call: suspended for good\n",
                         proc->serial, proc->name, (int)tid );
            }
        }
        if ((action == IOS_LEFTOVER_RUNNING || action == IOS_LEFTOVER_LOCKED) && !logged++)
            ios_log( "process %u (%s): thread %04x %s: the process's memory stays\n", proc->serial, proc->name,
                     (int)tid, action == IOS_LEFTOVER_RUNNING ? "runs (guest code)" : "is inside Wine" );
        if (action != IOS_LEFTOVER_GONE) ios_leftover_count( &counts, action );
    }
    counts.left = counts.woken - remaining;
    if (counts.left < 0) counts.left = 0;
    ios_log( "process %u (%s) ended with threads left: %d woken, %d left, %d suspended, %d kept\n",
             proc->serial, proc->name, counts.woken, counts.left, counts.suspended, counts.kept );
}


/***********************************************************************
 *           ios_end_pseudo_process
 *
 * Part D1: NtTerminateProcess on the own process (upstream's exit_process/abort_process).
 * Every other thread is alerted and leaves through the normal thread exit; then this thread
 * leaves, last, and the process ends.
 */
void ios_end_pseudo_process( int exit_code )
{
    struct ios_proc *proc = ios_proc_current();
    struct ntdll_thread_data *data;
    DWORD self;
    int waited;

    if (!ios_current_teb()) native_thread_exit( exit_code );
    data = ntdll_get_thread_data();
    if (data->ios_leaving) ios_thread_leave( exit_code );
    self = GetCurrentThreadId();

    if (InterlockedCompareExchange( &proc->state, IOS_PROC_ENDING, IOS_PROC_RUNNING ) != IOS_PROC_RUNNING &&
        InterlockedCompareExchange( &proc->state, IOS_PROC_ENDING, IOS_PROC_BOOTING ) != IOS_PROC_BOOTING)
    {
        /* another thread is already ending it: leave like any other thread */
        ios_thread_leave( exit_code );
    }
    proc->exit_code = exit_code;

    for (waited = 0; __atomic_load_n( &proc->pp_nb_threads, __ATOMIC_ACQUIRE ) > 1 &&
                     waited < IOS_END_WAIT_MS; waited++)
    {
        if (!(waited % IOS_END_REALERT_MS)) alert_process_threads( proc, self );
        usleep( 1000 );
    }

    /* spec 22 C5: threads asleep where no server reply reaches them */
    if (__atomic_load_n( &proc->pp_nb_threads, __ATOMIC_ACQUIRE ) > 1)
        forget_leftover_threads( proc, self );

    if (__atomic_load_n( &proc->pp_nb_threads, __ATOMIC_ACQUIRE ) > 1)
    {
        /* some threads didn't leave (busy in code that makes no system call): end the process
         * without them; its memory is handed back when the last of them leaves */
        ios_log( "process %u (%s): %d thread(s) didn't leave within %d ms\n", proc->serial,
                 proc->name, (int)proc->pp_nb_threads - 1, IOS_END_WAIT_MS + IOS_LEFTOVER_WAIT_MS );
        finish_process( proc, exit_code );
        mark_dead_early( proc );
    }
    ios_thread_leave( exit_code );
}

#endif /* WINE_IOS */
