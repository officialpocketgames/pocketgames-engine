// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08: small pieces of the in-app wineserver that the modified upstream files call into.

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/semaphore.h>

#include "ios_server.h"

/* contract Section A */
int wineserver_ready = 0;

/* ---- Part B3: kill ---- */

int ios_kill( pid_t pid, int sig )
{
    /* Every pseudo-process has the app's pid, so a signal "to a process" would hit the whole
     * app. Process groups (pid <= 0) would too. Nothing else exists to be signalled. */
    if (pid <= 0 || pid == getpid()) return 0;
    return kill( pid, sig );
}

/* ---- Part B5: NLS directory ---- */

static char *nls_dir;

void wineserver_set_nls_dir( const char *path )
{
    char *copy = (path && *path) ? strdup( path ) : NULL;
    free( nls_dir );
    nls_dir = copy;
}

const char *ios_nls_dir(void)
{
    return nls_dir;
}

/* ---- Part B1: leaving the main loop instead of exiting ---- */

static int leave_loop;

void ios_begin_run(void)
{
    leave_loop = 0;
}

void ios_request_loop_exit( const char *why )
{
    if (!leave_loop) ws_log( "main loop will stop: %s", why );
    leave_loop = 1;
}

int ios_should_leave_loop(void)
{
    if (leave_loop) return 1;
    return __atomic_load_n( (int *)&g_wineserver_should_stop, __ATOMIC_ACQUIRE ) != 0;
}

/* ---- Part B6: ready flag ---- */

void ios_set_ready(void)
{
    __atomic_store_n( &wineserver_ready, 1, __ATOMIC_RELEASE );
    ws_log( "ready (pid %d)", (int)getpid() );
}

/* ---- Part B2: signals ---- */

void ios_block_server_signals(void)
{
    sigset_t all;

    /* The Windows threads' handlers assume a Windows thread; none may run on this one. */
    sigfillset( &all );
    pthread_sigmask( SIG_BLOCK, &all, NULL );
}

/* ---- Part C1: injected client connections ---- */

static pthread_mutex_t inject_mutex = PTHREAD_MUTEX_INITIALIZER;
static int *inject_fds;        /* FIFO: inject_fds[inject_head .. inject_count-1] */
static size_t inject_head, inject_count, inject_size;

void wineserver_inject_client_fd( int fd )
{
    int queued = 0;

    if (fd < 0) return;

    pthread_mutex_lock( &inject_mutex );
    if (inject_head == inject_count) inject_head = inject_count = 0;  /* empty: start over */
    if (inject_count == inject_size && inject_head)  /* full at the end: move to the front */
    {
        memmove( inject_fds, inject_fds + inject_head, (inject_count - inject_head) * sizeof(*inject_fds) );
        inject_count -= inject_head;
        inject_head = 0;
    }
    if (inject_count == inject_size)
    {
        size_t new_size = inject_size ? inject_size * 2 : 8;
        int *new_fds = realloc( inject_fds, new_size * sizeof(*new_fds) );
        if (new_fds)
        {
            inject_fds = new_fds;
            inject_size = new_size;
        }
    }
    if (inject_count < inject_size)
    {
        inject_fds[inject_count++] = fd;
        queued = 1;
    }
    pthread_mutex_unlock( &inject_mutex );

    if (!queued)
    {
        ws_log( "client connection fd %d: out of memory, closed", fd );
        close( fd );
        return;
    }
    ws_log( "client connection fd %d queued", fd );
    wineserver_request_wake();
}

int ios_take_injected_fd(void)
{
    int fd = -1;

    pthread_mutex_lock( &inject_mutex );
    if (inject_head < inject_count) fd = inject_fds[inject_head++];
    pthread_mutex_unlock( &inject_mutex );
    return fd;
}

/* ---- Part D3: the wake-up ---- */

static pthread_once_t wake_once = PTHREAD_ONCE_INIT;
static semaphore_t wake_sem = MACH_PORT_NULL;

static void create_wake_sem(void)
{
    if (semaphore_create( mach_task_self(), &wake_sem, SYNC_POLICY_FIFO, 0 ) != KERN_SUCCESS)
        wake_sem = MACH_PORT_NULL;
}

/* Spec 09's client side calls this right after writing every request. */
void wineserver_request_wake(void)
{
    pthread_once( &wake_once, create_wake_sem );
    if (wake_sem != MACH_PORT_NULL) semaphore_signal( wake_sem );
}

void ios_wait_for_wake( long long nsec )
{
    mach_timespec_t ts;

    pthread_once( &wake_once, create_wake_sem );
    if (nsec <= 0) return;
    ts.tv_sec = (unsigned int)(nsec / 1000000000);
    ts.tv_nsec = (clock_res_t)(nsec % 1000000000);
    if (wake_sem != MACH_PORT_NULL) semaphore_timedwait( wake_sem, ts );
    else
    {
        struct timespec rq = { ts.tv_sec, ts.tv_nsec };
        nanosleep( &rq, NULL );
    }
}

void ios_drain_wakes(void)
{
    mach_timespec_t zero = { 0, 0 };
    int i;

    if (wake_sem == MACH_PORT_NULL) return;
    /* bounded: signals that keep coming only cost the next iteration its sleep */
    for (i = 0; i < 4096; i++)
        if (semaphore_timedwait( wake_sem, zero ) != KERN_SUCCESS) break;
}

/* ---- Part F5: the screen size the app reports ---- */

static long long screen_size = -1;   /* width << 32 | height; -1: not set by the app */
static int screen_changed;

/* The app calls this when the display mode changes (QUESTIONS.md, spec 08 question 3). */
void wineserver_set_screen_size( int width, int height )
{
    if (width <= 0 || height <= 0) return;
    __atomic_store_n( &screen_size, ((long long)width << 32) | (unsigned int)height, __ATOMIC_RELEASE );
    __atomic_store_n( &screen_changed, 1, __ATOMIC_RELEASE );
    wineserver_request_wake();
}

int ios_get_screen_size( int *width, int *height )
{
    static int env_width = -1, env_height = -1;
    long long size = __atomic_load_n( &screen_size, __ATOMIC_ACQUIRE );

    if (size > 0)
    {
        *width = (int)(size >> 32);
        *height = (int)(size & 0xffffffff);
        return 1;
    }
    if (env_width == -1)  /* read once, on the server thread */
    {
        env_width = ios_env_int( "PG_SCREEN_W", 0 );
        env_height = ios_env_int( "PG_SCREEN_H", 0 );
    }
    if (env_width <= 0 || env_height <= 0) return 0;
    *width = env_width;
    *height = env_height;
    return 1;
}

int ios_take_screen_change(void)
{
    return __atomic_exchange_n( &screen_changed, 0, __ATOMIC_ACQ_REL );
}

/* ---- Part G1: force quit ---- */

int g_wineserver_force_quits = 0;
static int force_quit_pending;
static int force_quit_code;

/* any thread: record the request, count it, wake the loop */
void wineserver_kill_programs( int exit_code )
{
    __atomic_store_n( &force_quit_code, exit_code, __ATOMIC_RELAXED );
    __atomic_store_n( &force_quit_pending, 1, __ATOMIC_RELEASE );
    __atomic_add_fetch( &g_wineserver_force_quits, 1, __ATOMIC_ACQ_REL );
    wineserver_request_wake();
}

int ios_take_force_quit( int *exit_code )
{
    if (!__atomic_exchange_n( &force_quit_pending, 0, __ATOMIC_ACQ_REL )) return 0;
    *exit_code = __atomic_load_n( &force_quit_code, __ATOMIC_RELAXED );
    return 1;
}

/* Part G1/G3: tell a client thread that something happened while it may be asleep where no
 * server reply reaches it (spec 09's per-thread alert; QUESTIONS.md, spec 08 question 4) */
void (*wineserver_thread_alert_hook)( unsigned int unix_tid ) = NULL;

int ios_alert_thread( int unix_tid )
{
    void (*hook)( unsigned int ) = __atomic_load_n( &wineserver_thread_alert_hook, __ATOMIC_ACQUIRE );

    if (!hook || unix_tid == -1) return 0;
    hook( (unsigned int)unix_tid );
    return 1;
}

/* ---- settings ---- */

int ios_env_int( const char *name, int def )
{
    const char *str = getenv( name );
    char *end;
    long val;

    if (!str || !*str) return def;
    errno = 0;
    val = strtol( str, &end, 10 );
    if (errno || *end || val < -0x7fffffffL || val > 0x7fffffffL) return def;
    return (int)val;
}
