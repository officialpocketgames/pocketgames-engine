// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: the few ntdll functions the audio driver calls, for the host tests. Built with
// Wine's headers so the prototypes (and calling convention) are the real ones.

#if 0
#pragma makedep unix
#endif

#include <pthread.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "test_stubs.h"

int test_events_set;

NTSTATUS WINAPI NtSetEvent( HANDLE handle, LONG *prev )
{
    __atomic_add_fetch( &test_events_set, 1, __ATOMIC_RELAXED );
    return STATUS_SUCCESS;
}

/* the timer thread a test started for TEST_TIMER_THREAD_HANDLE: waiting on the handle joins it */
pthread_t test_timer_thread;
int test_timer_thread_running;

NTSTATUS WINAPI NtWaitForSingleObject( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    if (handle == TEST_TIMER_THREAD_HANDLE && test_timer_thread_running)
    {
        pthread_join( test_timer_thread, NULL );
        test_timer_thread_running = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtClose( HANDLE handle )
{
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtAllocateVirtualMemory( HANDLE process, PVOID *addr, ULONG_PTR zero_bits, SIZE_T *size,
                                         ULONG type, ULONG protect )
{
    void *p = test_arena_alloc( *size );   /* the fake guest window */

    if (!p) return STATUS_NO_MEMORY;
    *addr = p;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtFreeVirtualMemory( HANDLE process, PVOID *addr, SIZE_T *size, ULONG type )
{
    return STATUS_SUCCESS;   /* the arena is reset per test */
}
