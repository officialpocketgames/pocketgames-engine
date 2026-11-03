// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35: opengl32's unix side shared by every pseudo-process.
//
// - B1: PG_OPENGL, read once per pseudo-process at process_attach; the process's answer.
// - D7: what upstream keeps per process in globals of unix_wgl.c, per process here (the debug
//   callback, the WoW64 strings, opengl32's context records) or per thread (zero_bits).
// - G1: the guest-window helpers the macros of unix_private.h use.
// - H1: opengl_ios_thread_exit, called by spec 09 E5's thread exit (weak there).
// - H2: opengl_ios_process_ended, called by spec 09 D1 once all of a program's threads are gone.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "unixlib.h"
#include "unix_private.h"
#include "opengl_ios.h"
#include "pgl.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(opengl);

extern void *ios_current_peb( void );          /* spec 09 B1 */
extern BOOL w32ios_opengl_installed( void );   /* spec 35 D1: win32u's iOS driver is installed */
/* unix_wgl.c */
extern BOOL wrap_wglMakeCurrent( TEB *teb, HDC hdc, HGLRC client_context );

void opengl_ios_log_line( const char *line )
{
    char buffer[2112];
    snprintf( buffer, sizeof(buffer), "%s\n", line );
    __wine_dbg_output( buffer );
}

/* ---------------------------------------------------------------- per process */

struct opengl_ios_proc
{
    void *_Atomic peb;               /* the key, written last; NULL: a free slot */
    int on;                          /* B1 */
    BOOL logged_off;                 /* clarification 8's line, once */
    UINT64 debug_callback;           /* D7 */
    void *strings;                   /* D7, G1.4: unix_wgl.c's struct wow64_string_entry[] */
    SIZE_T strings_count;
    struct list contexts;            /* H2: opengl32's struct context records */
};

#define OPENGL_IOS_PROCS 64
static struct opengl_ios_proc procs[OPENGL_IOS_PROCS];
static pthread_mutex_t procs_lock = PTHREAD_MUTEX_INITIALIZER;

/* lock-free: a slot is written fully before its key is published */
static struct opengl_ios_proc *proc_find( const void *peb )
{
    unsigned int i;

    if (!peb) return NULL;
    for (i = 0; i < OPENGL_IOS_PROCS; i++)
        if (atomic_load_explicit( &procs[i].peb, memory_order_acquire ) == peb) return &procs[i];
    return NULL;
}

static struct opengl_ios_proc *proc_current( void )
{
    return proc_find( ios_current_peb() );
}

/* process_attach: the slot of the calling process, written afresh (a PEB is reused once its
 * process ended: whatever a process that never reached H2 left there goes) */
static struct opengl_ios_proc *proc_attach( void *peb, int on )
{
    struct opengl_ios_proc *proc;
    unsigned int i;

    if (!peb) return NULL;
    pthread_mutex_lock( &procs_lock );
    if ((proc = proc_find( peb ))) atomic_store_explicit( &proc->peb, NULL, memory_order_release );
    else
    {
        for (i = 0; i < OPENGL_IOS_PROCS; i++) if (!atomic_load( &procs[i].peb )) break;
        proc = i < OPENGL_IOS_PROCS ? &procs[i] : NULL;
    }
    if (proc)
    {
        free( proc->strings );
        proc->on = on;
        proc->logged_off = FALSE;
        proc->debug_callback = 0;
        proc->strings = NULL;
        proc->strings_count = 0;
        list_init( &proc->contexts );
        atomic_store_explicit( &proc->peb, peb, memory_order_release );
    }
    pthread_mutex_unlock( &procs_lock );
    if (!proc) ERR( "more than %u programs use opengl32 at once\n", OPENGL_IOS_PROCS );
    return proc;
}

/* ---------------------------------------------------------------- B1 */

NTSTATUS opengl_ios_process_attach( int wow64, void *args )
{
    struct opengl_ios_proc *proc;
    const char *why = NULL;
    int on = opengl_ios_switch_on( getenv( "PG_OPENGL" ) );
    char line[192];

    if (on && !opengl32_upstream_present)
    {
        on = 0;
        why = "this build of opengl32 has no thunks";
    }
    /* the driver of Part D is installed at win32u's first use of OpenGL (once per app) */
    if (on && (!__wine_get_opengl_driver( WINE_OPENGL_DRIVER_VERSION ) || !w32ios_opengl_installed()))
    {
        on = 0;
        why = "win32u has no iOS GL driver";
    }
    proc = proc_attach( ios_current_peb(), on );
    if (on && !proc)
    {
        on = 0;
        why = "no slot for the program";
    }
    opengl_ios_thread_set( on ? 1 : -1 );

    if (on) opengl_ios_log_line( "[opengl] on (gl4es 2.1 on ANGLE/Metal)" );
    else
    {
        snprintf( line, sizeof(line), "[opengl] off%s%s", why ? ": " : "", why ? why : "" );
        opengl_ios_log_line( line );
    }
    if (!on) return STATUS_SUCCESS;

    opengl_ios_stats_init( opengl_ios_switch_on( getenv( "PG_GL_CALL_STATS" ) ) );
    return pgl_call_in_pool( (int32_t (*)( void * ))(wow64 ? opengl32_upstream_wow64_funcs : opengl32_upstream_funcs)[unix_process_attach],
                             args );
}

int opengl_ios_thread_resolve( void )
{
    struct opengl_ios_proc *proc = proc_current();
    int on = proc && proc->on ? 1 : -1;

    opengl_ios_thread_set( on );
    return on;
}

NTSTATUS opengl_ios_off_call( unsigned int index )
{
    struct opengl_ios_proc *proc;
    char line[128];

    if (index <= OPENGL_IOS_PROCESS_DETACH) return STATUS_SUCCESS;
    /* spec 12 clarification 8's line, once per pseudo-process */
    if ((proc = proc_current()) && !proc->logged_off)
    {
        proc->logged_off = TRUE;
        snprintf( line, sizeof(line), "[opengl32] no OpenGL on iOS: %u returns STATUS_NOT_SUPPORTED", index );
        opengl_ios_log_line( line );
    }
    return opengl_ios_off_status( index );
}

/* ---------------------------------------------------------------- D7 */

static _Thread_local ULONG_PTR tls_zero_bits;
static _Thread_local BOOL tls_zero_bits_known;

ULONG_PTR opengl_ios_zero_bits( void )
{
    if (!tls_zero_bits_known)
    {
        tls_zero_bits = 0;
        if (NtCurrentTeb()->WowTebOffset)
        {
            SYSTEM_BASIC_INFORMATION info;

            /* a guest ceiling: spec 10 E4 places the allocation inside the caller's window */
            if (!NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL ))
                tls_zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
            else
                tls_zero_bits = 0x7fffffff;
        }
        tls_zero_bits_known = TRUE;
    }
    return tls_zero_bits;
}

UINT64 opengl_ios_debug_callback( void )
{
    struct opengl_ios_proc *proc = proc_current();
    return proc ? proc->debug_callback : 0;
}

void opengl_ios_set_debug_callback( UINT64 callback )
{
    struct opengl_ios_proc *proc = proc_current();
    if (proc) proc->debug_callback = callback;
}

/* with wgl_lock held: a process without a slot shares one table that is never freed */
void **opengl_ios_wow64_strings( SIZE_T **count )
{
    static void *orphan_strings;
    static SIZE_T orphan_count;
    struct opengl_ios_proc *proc = proc_current();

    if (!proc)
    {
        *count = &orphan_count;
        return &orphan_strings;
    }
    *count = &proc->strings_count;
    return &proc->strings;
}

void opengl_ios_context_created( struct list *entry )
{
    struct opengl_ios_proc *proc = proc_current();

    pthread_mutex_lock( &wgl_lock );
    if (proc) list_add_tail( &proc->contexts, entry );
    else list_init( entry );
    pthread_mutex_unlock( &wgl_lock );
}

void opengl_ios_context_deleted( struct list *entry )
{
    pthread_mutex_lock( &wgl_lock );
    if (entry->next) list_remove( entry );
    list_init( entry );
    pthread_mutex_unlock( &wgl_lock );
}

/* ---------------------------------------------------------------- G1 */

BOOL opengl_ios_in_window( const void *ptr )
{
    int outside;

    if (!ptr || !ios_wow_host_base()) return FALSE;
    opengl_wow_guest_at( ios_wow_host_base(), ptr, &outside );
    return !outside;
}

void opengl_ios_wow_outside( const void *host )
{
    static _Atomic unsigned int count;
    unsigned int n = atomic_fetch_add( &count, 1 );
    char line[160];

    if (n >= 8 && n % 256) return;
    snprintf( line, sizeof(line), "[opengl] a host pointer %p outside the program's guest window was handed back as NULL (%u times)",
              host, n + 1 );
    opengl_ios_log_line( line );
}

/* ---------------------------------------------------------------- H1 */

static NTSTATUS release_current( void *arg )
{
    TEB *teb = arg;
    wrap_wglMakeCurrent( teb, 0, NULL );
    return STATUS_SUCCESS;
}

/* spec 09 E5: on the exiting thread, while its TEB and server connection still work */
void opengl_ios_thread_exit( void )
{
    int on = opengl_ios_thread_get();
    TEB *teb;

    /* a host thread may boot another program later (spec 22): nothing per thread stays */
    opengl_ios_thread_set( 0 );
    tls_zero_bits_known = FALSE;
    if (on <= 0) return;

    /* as wglMakeCurrent( NULL, NULL ): win32u releases the drawables, pgl the ANGLE context */
    if ((teb = NtCurrentTeb()) && teb->glContext) pgl_call_in_pool( (int32_t (*)( void * ))release_current, teb );
    pgl_thread_exit();
}

/* ---------------------------------------------------------------- H2 */

/* spec 09 D1: all of the program's threads are gone (no TEB on this thread any more), its
 * memory not given back yet */
void opengl_ios_process_ended( void *peb )
{
    struct opengl_ios_proc *proc;
    struct list contexts = LIST_INIT( contexts );
    unsigned int n_contexts = 0, pgl_contexts = 0, pgl_drawables = 0;
    SIZE_T n_strings = 0;
    void *strings = NULL;
    char line[192];

    if (!(proc = proc_find( peb ))) return;

    pthread_mutex_lock( &wgl_lock );
    list_move_tail( &contexts, &proc->contexts );
    strings = proc->strings;
    n_strings = proc->strings_count;
    proc->strings = NULL;
    proc->strings_count = 0;
    pthread_mutex_unlock( &wgl_lock );

    opengl_ios_free_contexts( &contexts, &n_contexts );
    free( strings );
    if (proc->on) pgl_process_ended( peb, &pgl_contexts, &pgl_drawables );

    pthread_mutex_lock( &procs_lock );
    atomic_store_explicit( &proc->peb, NULL, memory_order_release );
    pthread_mutex_unlock( &procs_lock );

    if (proc->on || n_contexts || n_strings)
    {
        snprintf( line, sizeof(line),
                  "[opengl] program %p ended: %u contexts, %lu WoW64 strings freed; pgl: %u contexts, %u drawables",
                  peb, n_contexts, (unsigned long)n_strings, pgl_contexts, pgl_drawables );
        opengl_ios_log_line( line );
    }
}
