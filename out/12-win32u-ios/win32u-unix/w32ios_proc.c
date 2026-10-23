// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part B (spec 09 Part B applied to win32u): every pseudo-process shares one copy of
// win32u, so what upstream keeps per process lives in a struct w32ios_proc per pseudo-process.
//
// The registry: W32IOS_PP_SLOTS slots keyed by (PEB, process id), found without a lock
// (w32ios_pp_find, a seqlock per slot); created under table_lock, written fully before they are
// published. A slot is recycled when its process has ended: when a new process has the same PEB
// (two live processes can't share an address) or, when every slot is taken, after asking the
// server whether the process still runs. Each thread caches its slot, keyed by its TEB, PEB and
// process id (a pthread can outlive a Windows thread and later carry another one).
//
// Also B1 (the allocation ceiling per thread) and B9 (the shared data page).

#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#include "winternl.h"
#include "ddk/wdm.h"   /* KUSER_SHARED_DATA.NtSystemRoot (clarification 10); before win32u's headers, as message.c */
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "w32ios.h"

static struct w32ios_pp_table table;
static struct w32ios_proc procs[W32IOS_PP_SLOTS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

/* used when every slot holds a live process, and on a thread without TEB */
static struct w32ios_proc overflow_proc;
static pthread_once_t overflow_once = PTHREAD_ONCE_INIT;

static void *session_peb;
static LONG session_ready;

struct thread_cache
{
    TEB  *teb;
    void *peb;
    DWORD pid;
    struct w32ios_proc *proc;
};
static __thread struct thread_cache cache;

/* write a slot's initial state; the slot is unpublished (or not in the table) */
static void init_proc( struct w32ios_proc *proc )
{
    RTL_USER_PROCESS_PARAMETERS *params = NULL;
    TEB *teb = NtCurrentTeb();

    /* the previous owner is dead: its handles and memory went with it, only our own heap
     * blocks are freed. Nobody holds its builtins_lock (no thread of it is left), so the
     * mutex is simply initialised again. */
    free( proc->pp_registered_devices );

    memset( proc, 0, sizeof(*proc) );
    pthread_mutex_init( &proc->builtins_lock, NULL );
    list_init( &proc->pp_class_list );
    list_init( &proc->pp_dce_list );
    list_init( &proc->pp_window_surfaces );
    list_init( &proc->pp_dc_attr_buckets );
    list_init( &proc->pp_icon_cache );
    list_init( &proc->pp_client_surfaces );
    proc->pp_process_layout = ~0u;            /* sysparams.c: not queried yet */
    proc->pp_enable_mouse_in_pointer = -1;    /* input.c: not set */
    proc->pp_caret.timeout = 500;             /* input.c: the default blink time */

    /* window.c's init_startup_info for this process */
    if (teb && teb->Peb) params = teb->Peb->ProcessParameters;
    if (params)
    {
        proc->pp_startup_show_window = params->wShowWindow;
        proc->pp_startup_info_flags = params->dwFlags;
    }
}

static void init_overflow( void )
{
    init_proc( &overflow_proc );
}

/* is_dead callback of w32ios_pp_pick: does the server still know a running process with
 * this id and PEB? */
static int process_ended( void *peb, uint32_t pid, void *ctx )
{
    OBJECT_ATTRIBUTES attr = { sizeof(attr) };
    CLIENT_ID cid = { .UniqueProcess = ULongToHandle( pid ) };
    PROCESS_BASIC_INFORMATION info;
    HANDLE handle;
    NTSTATUS status;

    if (NtOpenProcess( &handle, PROCESS_QUERY_LIMITED_INFORMATION, &attr, &cid )) return 1;
    status = NtQueryInformationProcess( handle, ProcessBasicInformation, &info, sizeof(info), NULL );
    NtClose( handle );
    if (status) return 0;
    return info.ExitStatus != STATUS_PENDING || info.PebBaseAddress != peb;
}

/* Spec 12 clarification 6: a session starts with the calling process when it has no parent.
 * Only the program the app boots through __wine_main has none (spec 08 C1); every child
 * (wineboot, system processes, a game started by its launcher) has one. */
static BOOL calling_process_is_session_root( void )
{
    PROCESS_BASIC_INFORMATION info;
    NTSTATUS status;

    status = NtQueryInformationProcess( GetCurrentProcess(), ProcessBasicInformation, &info, sizeof(info), NULL );
    return w32ios_is_session_root( !status, (uint64_t)info.InheritedFromUniqueProcessId );
}

/* Spec 12 clarification 10: is another user program of the session running? Every process the
 * server lists with a running thread counts, but the caller, images inside the Windows directory
 * (the session's system processes) and helpers (w32ios_is_user_program). TRUE as well when the
 * list can't be read: the size then stays, the safe side for a running program. */
BOOL w32ios_other_user_program_running( void )
{
    const struct _KUSER_SHARED_DATA *usd = w32ios_usd();
    const WCHAR *root = usd->NtSystemRoot;
    DWORD self = GetCurrentProcessId();
    SYSTEM_PROCESS_INFORMATION *spi;
    ULONG size = 0x8000, len = 0, pos = 0;
    char *buffer = NULL;
    NTSTATUS status;
    BOOL found = FALSE;

    for (;;)
    {
        free( buffer );
        if (!(buffer = malloc( size ))) return TRUE;
        status = NtQuerySystemInformation( SystemProcessInformation, buffer, size, &len );
        if (status != STATUS_INFO_LENGTH_MISMATCH) break;
        size = max( size * 2, len + 0x1000 );
    }
    if (status)
    {
        w32ios_log( "cannot list the processes (%#x): the virtual monitor keeps its size", (unsigned int)status );
        free( buffer );
        return TRUE;
    }

    for (;;)
    {
        OBJECT_ATTRIBUTES attr = { sizeof(attr) };
        CLIENT_ID cid = { 0 };
        WCHAR path_buffer[sizeof(UNICODE_STRING) / sizeof(WCHAR) + MAX_PATH + 1];
        UNICODE_STRING *path = (UNICODE_STRING *)path_buffer;
        HANDLE handle;
        DWORD pid;

        spi = (SYSTEM_PROCESS_INFORMATION *)(buffer + pos);
        pid = HandleToULong( spi->UniqueProcessId );
        if (spi->dwThreadCount && pid && pid != self)
        {
            path->Length = 0;
            path->Buffer = NULL;
            cid.UniqueProcess = spi->UniqueProcessId;
            if (!NtOpenProcess( &handle, PROCESS_QUERY_LIMITED_INFORMATION, &attr, &cid ))
            {
                if (NtQueryInformationProcess( handle, ProcessImageFileNameWin32, path_buffer,
                                               sizeof(path_buffer) - sizeof(WCHAR), NULL ))
                    path->Length = 0;
                NtClose( handle );
            }
            if (w32ios_is_user_program( path->Length ? path->Buffer : NULL, path->Length / sizeof(WCHAR),
                                        root, wcslen( root ), spi->ProcessName.Buffer,
                                        spi->ProcessName.Length / sizeof(WCHAR) ))
            {
                w32ios_log_verbose( "process %04x (%s) is running", (unsigned int)pid,
                                    debugstr_us( &spi->ProcessName ) );
                found = TRUE;
                break;
            }
        }
        if (!spi->NextEntryOffset) break;
        pos += spi->NextEntryOffset;
    }
    free( buffer );
    return found;
}

static struct w32ios_proc *create_proc( void *peb, DWORD pid )
{
    struct w32ios_proc *proc;
    int index, reuse;
    BOOL root = calling_process_is_session_root(), new_session = FALSE;

    pthread_mutex_lock( &table_lock );
    index = w32ios_pp_pick( &table, peb, pid, process_ended, NULL, &reuse );
    if (index < 0)
    {
        static LONG warned;
        pthread_mutex_unlock( &table_lock );
        if (!InterlockedExchange( &warned, 1 ))
            w32ios_log( "per-process table full (%u live processes): process %04x shares a slot",
                        W32IOS_PP_SLOTS, (unsigned int)pid );
        pthread_once( &overflow_once, init_overflow );
        return &overflow_proc;
    }
    proc = &procs[index];
    if (!reuse)
    {
        /* a process with no parent starts a new session (Part C2: the monitor's size goes
         * back to the session default) */
        new_session = root;
        w32ios_pp_begin( &table, index );
        init_proc( proc );
        w32ios_pp_publish( &table, index, peb, pid );
        w32ios_log_verbose( "process %04x (PEB %p) uses slot %d%s", (unsigned int)pid, peb, index,
                            new_session ? ", new session" : "" );
    }
    pthread_mutex_unlock( &table_lock );

    if (new_session) w32ios_session_start();
    if (!reuse) w32ios_gdi_attach_proc( proc );
    return proc;
}

struct w32ios_proc *w32ios_proc_current( void )
{
    TEB *teb = NtCurrentTeb();
    struct w32ios_proc *proc;
    void *peb;
    DWORD pid;
    int index;

    if (!teb)
    {
        pthread_once( &overflow_once, init_overflow );
        return &overflow_proc;
    }
    peb = ios_current_peb();
    pid = HandleToULong( teb->ClientId.UniqueProcess );
    if (cache.teb == teb && cache.peb == peb && cache.pid == pid) return cache.proc;

    if ((index = w32ios_pp_find( &table, peb, pid )) >= 0) proc = &procs[index];
    else proc = create_proc( peb, pid );

    cache.teb = teb;
    cache.peb = peb;
    cache.pid = pid;
    cache.proc = proc;
    return proc;
}

void w32ios_set_session_process( void *peb )
{
    session_peb = peb;
    WriteRelease( &session_ready, 1 );
    w32ios_log( "session's first process initialised win32u (PEB %p)", peb );
}

void *w32ios_session_peb( void )
{
    return ReadAcquire( &session_ready ) ? session_peb : NULL;
}

BOOL w32ios_session_initialized( void )
{
    return ReadAcquire( &session_ready ) != 0;
}

/* ---------------------------------------------------------------- B1 */

struct ceiling_cache
{
    TEB      *teb;
    ULONG_PTR value;
};
static __thread struct ceiling_cache ceiling;

ULONG_PTR w32ios_zero_bits( void )
{
    TEB *teb = NtCurrentTeb();
    SYSTEM_BASIC_INFORMATION info;
    ULONG_PTR value = 0;

    if (!teb) return 0;
    if (ceiling.teb == teb) return ceiling.value;   /* a thread never changes process */

    if (teb->WowTebOffset &&
        !NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL ))
        value = w32ios_ceiling( 1, (ULONG_PTR)info.HighestUserAddress );
    ceiling.value = value;
    ceiling.teb = teb;
    return value;
}

/* ---------------------------------------------------------------- B9 */

static const LONG64 zero_page[4096 / sizeof(LONG64)];

const void *w32ios_usd( void )
{
    unsigned long long real = ios_get_real_usd();
    return real ? (const void *)(ULONG_PTR)real : (const void *)zero_page;
}
