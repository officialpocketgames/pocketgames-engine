// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part B, memory that upstream allocates once per process but that here is shared by
// every pseudo-process, so it must not belong to the first process (spec 09 Part D1 hands a
// dead process's memory back) and must be reachable by 32-bit code where 32-bit code reads it:
//
//  - the GDI handle table: allocated once, outside ntdll's bookkeeping; each process's PEB points
//    at it, a 32-bit process at a second mapping of the same pages inside its guest window
//    (32-bit gdi32 reads PEB64->GdiSharedHandleTable truncated to 32 bits);
//  - the views of the session's shared memory: replaced by mappings that outlive the process
//    that mapped them;
//  - NLS code page tables kept by font.c: copied out of the mapping process's views.

#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#include "win32u_private.h"
#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "wine/debug.h"
#include "w32ios.h"
#include "w32ios_mach.h"

static void *gdi_table;
static SIZE_T gdi_size;
static pthread_once_t gdi_once = PTHREAD_ONCE_INIT;

static void alloc_gdi_table( void )
{
    SIZE_T page = w32ios_page_size();

    gdi_size = (sizeof(GDI_SHARED_MEMORY) + page - 1) & ~(page - 1);
    if (!(gdi_table = w32ios_vm_alloc( gdi_size )))
        w32ios_log( "cannot allocate the GDI handle table (%lu bytes)", (unsigned long)gdi_size );
}

void *w32ios_gdi_table( SIZE_T *size )
{
    pthread_once( &gdi_once, alloc_gdi_table );
    if (size) *size = gdi_size;
    return gdi_table;
}

void w32ios_gdi_attach_proc( struct w32ios_proc *proc )
{
    if (InterlockedExchange( &proc->gdi_attached, 1 )) return;
    w32ios_gdi_publish_slow();
}

/* ---------------------------------------------------------------- spec 17 Part F */

/* F2: decided from PG_GDI_SHARED_SECTION when it is set; otherwise from the session's
 * arming (spec 10 clarification 3), which never goes back to 0: a "yes" is final, a "no" is
 * asked again (one acquire load) until the session arms. See QUESTIONS.md, spec 17 #5. */
static int session_32bit = -1;   /* -1: not read yet, 0: no (final), 1: yes (final), 2: not armed yet */

BOOL w32ios_gdi_session_32bit( void )
{
    int state = __atomic_load_n( &session_32bit, __ATOMIC_ACQUIRE );

    if (state == 0 || state == 1) return state;
    if (state < 0)
    {
        const char *value = getenv( "PG_GDI_SHARED_SECTION" );
        if (value && value[0])
        {
            state = w32ios_s17_session_32bit( value, 0 );
            __atomic_store_n( &session_32bit, state, __ATOMIC_RELEASE );
            w32ios_log( "GDI table shared with 32-bit programs: %s (PG_GDI_SHARED_SECTION=%s)",
                        state ? "yes" : "no", value );
            return state;
        }
    }
    if (w32ios_s17_session_32bit( NULL, ios_wow_session_armed ? ios_wow_session_armed() : -1 ))
    {
        __atomic_store_n( &session_32bit, 1, __ATOMIC_RELEASE );
        w32ios_log_verbose( "GDI table shared with 32-bit programs: yes (32-bit session)" );
        return TRUE;
    }
    __atomic_store_n( &session_32bit, 2, __ATOMIC_RELEASE );
    return FALSE;
}

void w32ios_s17_caller( struct w32ios_s17_owner *owner )
{
    TEB *teb = NtCurrentTeb();

    owner->pid = GetCurrentProcessId();
    owner->peb = teb ? teb->Peb : NULL;
    owner->is_32bit = teb && teb->WowTebOffset;
}

/* F4: the views of 32-bit processes, by (process id, PEB); a process whose view couldn't be
 * mapped is remembered with view NULL, so it is logged once and then given the master */
__thread const void *w32ios_gdi_published_peb;
static struct w32ios_s17_views gdi_views;
static pthread_mutex_t gdi_views_lock = PTHREAD_MUTEX_INITIALIZER;

static void *gdi_view_for_process( PEB *peb, void *table, SIZE_T size )
{
    DWORD pid = GetCurrentProcessId();
    void *view = NULL;
    int idx;

    pthread_mutex_lock( &gdi_views_lock );
    if ((idx = w32ios_s17_views_find( &gdi_views, pid, peb )) >= 0)
    {
        view = gdi_views.slot[idx].view;
        pthread_mutex_unlock( &gdi_views_lock );
        return view ? view : table;
    }
    /* reserve room below the caller's ceiling (spec 10 places it in the guest window), then
     * put a second mapping of the same pages over it: one table, two addresses */
    if (NtAllocateVirtualMemory( GetCurrentProcess(), &view, w32ios_zero_bits(), &size,
                                 MEM_RESERVE, PAGE_READWRITE ) ||
        !w32ios_vm_alias( table, size, &view, 1 ))
    {
        w32ios_log( "no GDI handle table inside the guest window of process %04x: publishing %p",
                    (unsigned int)pid, table );
        view = NULL;
    }
    else w32ios_log_verbose( "GDI handle table for 32-bit process %04x at %p", (unsigned int)pid, view );
    w32ios_s17_views_put( &gdi_views, pid, peb, view );
    pthread_mutex_unlock( &gdi_views_lock );
    return view ? view : table;
}

void w32ios_gdi_publish_slow( void )
{
    TEB *teb = NtCurrentTeb();
    void *table, *addr;
    SIZE_T size;
    PEB *peb;

    if (!teb || !(peb = teb->Peb)) return;
    if (!(addr = table = w32ios_gdi_table( &size ))) return;
    /* a 64-bit pseudo-process gets the master address, a 32-bit one a view in its window */
    if (w32ios_s17_publish_kind( teb->WowTebOffset != 0, w32ios_gdi_session_32bit() ) == W32IOS_S17_VIEW)
        addr = gdi_view_for_process( peb, table, size );
    peb->GdiSharedHandleTable = addr;
    w32ios_gdi_published_peb = peb;
}

void w32ios_detach_view( void **base, SIZE_T size )
{
    void *alias = NULL;

    if (!*base || !size) return;
    if (!w32ios_vm_alias( *base, size, &alias, 0 ))
    {
        static LONG warned;
        if (!InterlockedExchange( &warned, 1 ))
            w32ios_log( "cannot detach the session view %p from its process", *base );
        return;
    }
    NtUnmapViewOfSection( GetCurrentProcess(), *base );
    *base = alias;
}

const USHORT *w32ios_copy_nls( const USHORT *data, SIZE_T size )
{
    USHORT *copy;

    if (!data || !size || !(copy = malloc( size ))) return data;
    memcpy( copy, data, size );
    return copy;
}

void *w32ios_copy_view( void *view, SIZE_T size )
{
    void *copy;

    if (!view || !size || !(copy = malloc( size ))) return view;
    memcpy( copy, view, size );
    NtUnmapViewOfSection( GetCurrentProcess(), view );
    return copy;
}

void w32ios_init_cptable( const USHORT *data, CPTABLEINFO *info )
{
    void *ptr;
    SIZE_T size;

    if (!data) return;
    /* data[1] is the table's code page; ask for the same table again to learn its size */
    if (!NtGetNlsSectionPtr( 11, data[1], NULL, &ptr, &size ))
        data = w32ios_copy_nls( ptr, size );
    RtlInitCodePageTable( (USHORT *)data, info );
}
