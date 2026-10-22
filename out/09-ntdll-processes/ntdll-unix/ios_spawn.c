// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part C: starting programs. The session's first program boots on the thread that calls
// __wine_main (loader.c); a child boots on a new thread started here (NtCreateUserProcess in
// process.c prepares it). Also: the live-children count, the refused spawns, the program's
// working folder and the boot milestones.

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/qos.h>
#include <pthread/qos.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"
#include "ios_pure.h"

#ifdef WINE_IOS

/* stack of a boot thread: it runs the loader until the program's first thread switches to its
 * own Wine stacks */
#define IOS_BOOT_STACK_SIZE (8 * 1024 * 1024)

/***********************************************************************
 *           ios_monotonic_seconds
 */
double ios_monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec + ts.tv_nsec / 1e9;
}


/***********************************************************************
 *           ios_boot_phase
 *
 * Part C1: boot milestones of a program the app started (the session's first, and spec 22's
 * later ones) go to the app (winios_phase).
 */
void ios_boot_phase( const char *name )
{
    if (ios_proc_current()->top_level) winios_phase( name );
}


/***********************************************************************
 *           ios_set_thread_qos
 *
 * Part E1: every thread the unix side creates for Windows code is USER_INTERACTIVE, set
 * through the attribute (a thread can't raise its own class later).
 */
void ios_set_thread_qos( pthread_attr_t *attr )
{
    pthread_attr_set_qos_class_np( attr, QOS_CLASS_USER_INTERACTIVE, 0 );
}


/***********************************************************************
 *           child_boot_main
 *
 * The thread of a child pseudo-process: guest window first (32-bit), then the boot.
 */
static void *child_boot_main( void *arg )
{
    struct ios_proc *proc = arg;

    ios_set_thread_proc( proc );
    proc->boot_thread = pthread_self();
    ios_proc_set_stage( proc, IOS_STAGE_THREAD );

    if (proc->machine == IMAGE_FILE_MACHINE_I386 && !proc->il_only_64)
    {
        /* before the TEB and PEB: 32-bit code reads their addresses as 32-bit values */
        if (ios_mem_reserve_wow_window( proc )) ios_boot_failed( "no guest window", STATUS_NO_MEMORY );
        ios_proc_set_stage( proc, IOS_STAGE_WINDOW );
    }
    ios_boot_pseudo_process( proc );  /* never returns */
}


/***********************************************************************
 *           ios_start_boot_thread
 *
 * Part C4: starts the thread that boots a child. On failure the child's socket is closed and
 * nothing is left of it (the server sees the child end by EOF).
 */
NTSTATUS ios_start_boot_thread( struct ios_proc *proc )
{
    pthread_attr_t attr;
    pthread_t thread;
    int err;

    pthread_attr_init( &attr );
    pthread_attr_setstacksize( &attr, IOS_BOOT_STACK_SIZE );
    /* nothing joins it: like upstream's main thread it has no pthread_id in its thread data */
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    ios_set_thread_qos( &attr );
    err = pthread_create( &thread, &attr, child_boot_main, proc );
    pthread_attr_destroy( &attr );
    if (!err)
    {
        if (!ios_quiet) ios_log( "process %u (%s): child started\n", proc->serial, proc->name );
        return STATUS_SUCCESS;
    }

    ios_log( "process %u (%s): no boot thread: %s\n", proc->serial, proc->name, strerror( err ));
    if (proc->boot_socket != -1) close( proc->boot_socket );
    ios_children_remove( proc->child_id );
    ios_proc_free( proc );
    return STATUS_NO_MEMORY;
}


/***********************************************************************
 *           log_once_per_name
 */
static BOOL first_time_for( const char *name )
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static char *seen[64];
    static unsigned int count;
    unsigned int i;
    BOOL first = TRUE;

    pthread_mutex_lock( &mutex );
    for (i = 0; i < count; i++) if (!strcasecmp( seen[i], name )) first = FALSE;
    if (first && count < ARRAY_SIZE(seen)) seen[count++] = strdup( name );
    pthread_mutex_unlock( &mutex );
    return first;
}


/***********************************************************************
 *           ios_check_refused_spawn
 *
 * Part C6: STATUS_ACCESS_DENIED for optional helpers that can only fault here, and for
 * conhost.exe in an ARM64EC process unless MADEIRA_EC_CONHOST=1. The reason is logged once
 * per name.
 */
NTSTATUS ios_check_refused_spawn( const UNICODE_STRING *image )
{
    char path[MAX_PATH * 3];
    const char *token, *allow;
    int len;

    if (!image || !image->Buffer || !image->Length) return STATUS_SUCCESS;
    len = ntdll_wcstoumbs( image->Buffer, image->Length / sizeof(WCHAR), path, sizeof(path) - 1, FALSE );
    if (len <= 0) return STATUS_SUCCESS;
    path[len] = 0;

    if ((token = ios_refused_spawn_match( path )))
    {
        if (first_time_for( ios_file_part( path )))
            ios_log( "refused to start %s: an optional helper (%s) that can only fault on iOS\n",
                     ios_file_part( path ), token );
        return STATUS_ACCESS_DENIED;
    }
    if (is_arm64ec() && ios_is_conhost( path ))
    {
        allow = getenv( "MADEIRA_EC_CONHOST" );
        if (allow && !strcmp( allow, "1" )) return STATUS_SUCCESS;
        if (first_time_for( ios_file_part( path )))
            ios_log( "refused to start conhost.exe in an ARM64EC process: its threads would have no "
                     "CPU area for the x64 emulator (MADEIRA_EC_CONHOST=1 allows it)\n" );
        return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           ios_mark_system_process
 *
 * Part C5: the calling process made itself a Wine system process (services.exe, winedevice.exe
 * and the like); it no longer counts as a live game child.
 */
void ios_mark_system_process(void)
{
    ios_children_set_system( ios_proc_current()->child_id );
}


/***********************************************************************
 *           madeira_live_game_children   (contract Section A)
 *
 * Part C5: how many children are alive, not helpers, and started at most max_age seconds ago
 * (max_age < 0: any age); their names, comma-separated, go to buf.
 */
int madeira_live_game_children( char *buf, int len, double max_age )
{
    return ios_children_live( buf, len, max_age, ios_monotonic_seconds() );
}


/***********************************************************************
 *           ios_initial_directory
 *
 * Part C3: MADEIRA_INITIAL_CWD (a Windows path ending in '\') as the NT path the process
 * parameters are built from, or NULL when it isn't set.
 */
WCHAR *ios_initial_directory(void)
{
    static const WCHAR prefixW[] = {'\\','?','?','\\'};
    const char *cwd = getenv( "MADEIRA_INITIAL_CWD" );
    size_t len;
    WCHAR *ret;
    DWORD n;

    if (!cwd || !cwd[0]) return NULL;
    len = strlen( cwd );
    if (!(ret = malloc( (ARRAY_SIZE(prefixW) + len + 2) * sizeof(WCHAR) ))) return NULL;
    memcpy( ret, prefixW, sizeof(prefixW) );
    n = ntdll_umbstowcs( cwd, len, ret + ARRAY_SIZE(prefixW), len );
    n += ARRAY_SIZE(prefixW);
    if (ret[n - 1] != '\\') ret[n++] = '\\';
    ret[n] = 0;
    if (!ios_quiet) ios_log( "working folder %s\n", cwd );
    return ret;
}

#endif /* WINE_IOS */
