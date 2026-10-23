// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarifications 8 and 9: what the stand-in tables' unsupported entries do. They return
// STATUS_NOT_SUPPORTED; the first one a pseudo-process calls in a library logs
//     <head>: <entry index> returns STATUS_NOT_SUPPORTED
// for example "[opengl32] no OpenGL on iOS: 12 returns STATUS_NOT_SUPPORTED". A pseudo-process is
// its (PEB, process id), as for win32u's per-process slots (w32ios_proc.c), but no win32u slot is
// created for it: a program can use these libraries without any window.

#include <stdio.h>
#include <pthread.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wine/debug.h"
#include "unix_tables_ios.h"
#include "unix_tables_pure.h"

extern void *ios_current_peb( void );   /* spec 09 Part B1 */

static struct w32ios_unix_seen seen[W32IOS_UNIX_LIBS];
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

/* per library, the process this thread last reported for, so later calls skip the lock */
struct reported_cache
{
    TEB  *teb;
    void *peb;
    DWORD pid;
};
static __thread struct reported_cache reported[W32IOS_UNIX_LIBS];

NTSTATUS w32ios_unix_unsupported( const struct w32ios_unix_report *report, unsigned int index )
{
    TEB *teb = NtCurrentTeb();
    struct reported_cache *cache;
    char line[128];
    void *peb;
    DWORD pid;
    int first;

    if (!teb || report->id >= W32IOS_UNIX_LIBS) return STATUS_NOT_SUPPORTED;
    peb = ios_current_peb();
    pid = HandleToULong( teb->ClientId.UniqueProcess );
    cache = &reported[report->id];
    if (cache->teb == teb && cache->peb == peb && cache->pid == pid) return STATUS_NOT_SUPPORTED;

    pthread_mutex_lock( &seen_lock );
    first = w32ios_unix_first_report( &seen[report->id], peb, pid );
    pthread_mutex_unlock( &seen_lock );
    cache->teb = teb;
    cache->peb = peb;
    cache->pid = pid;

    if (first)
    {
        snprintf( line, sizeof(line), "%s: %u returns STATUS_NOT_SUPPORTED\n", report->head, index );
        __wine_dbg_output( line );
    }
    return STATUS_NOT_SUPPORTED;
}
