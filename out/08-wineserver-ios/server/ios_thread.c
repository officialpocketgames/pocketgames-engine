// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08: the process "tracing" interface of the server (process.h, thread.h) for the app,
// where every pseudo-process is a group of threads of the app's own task. It replaces
// upstream's mach.c / ptrace.c / procfs.c, which process.h no longer selects on iOS.

#include "config.h"

#include <errno.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/vm_map.h>  /* vm_read_overwrite, vm_write: <mach/mach_vm.h> is unsupported in the iPhoneOS SDK */

#include "ntstatus.h"
#include "winternl.h"

#include "file.h"
#include "process.h"
#include "thread.h"
#include "request.h"

#ifdef USE_IOS_THREADS

static void set_kern_error( kern_return_t kr )
{
    switch (kr)
    {
    case KERN_SUCCESS:            break;
    case KERN_INVALID_ARGUMENT:   set_error( STATUS_INVALID_PARAMETER ); break;
    case KERN_NO_SPACE:           set_error( STATUS_NO_MEMORY ); break;
    case KERN_PROTECTION_FAILURE: set_error( STATUS_ACCESS_DENIED ); break;
    case KERN_INVALID_ADDRESS:    set_error( STATUS_ACCESS_VIOLATION ); break;
    default:                      set_error( STATUS_UNSUCCESSFUL ); break;
    }
}

/* Nothing to set up: there are no other tasks to attach to. */
void init_tracing_mechanism(void) { }
void init_process_tracing( struct process *process ) { }
void finish_process_tracing( struct process *process ) { }

/* There are no child processes to reap. */
void sigchld_callback(void) { }

void init_thread_context( struct thread *thread ) { }

/* Only debug registers are ever handled on the server side (thread.c's system_flags), and,
 * as with upstream's Mach backend on arm64, they are not supported. */
void get_thread_context( struct thread *thread, struct context_data *context, unsigned int flags )
{
}

void set_thread_context( struct thread *thread, const struct context_data *context, unsigned int flags )
{
}

/* Signals sent to single threads of the app can't be handled the Windows way (Part B2);
 * none is ever sent. */
int send_thread_signal( struct thread *thread, int sig )
{
    return 0;
}

/* All pseudo-processes share the app's address space, so the memory of any of them is the
 * app's own memory. The Mach calls turn a bad address into an error instead of a crash. */
int read_process_memory( struct process *process, client_ptr_t ptr, data_size_t size, char *dest )
{
    vm_size_t done = 0;
    kern_return_t kr;

    if ((vm_address_t)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (!size) return 1;
    kr = vm_read_overwrite( mach_task_self(), (vm_address_t)ptr, size, (vm_address_t)(uintptr_t)dest, &done );
    if (kr == KERN_SUCCESS && done != size) kr = KERN_INVALID_ADDRESS;
    set_kern_error( kr );
    return kr == KERN_SUCCESS;
}

int write_process_memory( struct process *process, client_ptr_t ptr, data_size_t size, const char *src,
                          data_size_t *written )
{
    kern_return_t kr;

    if (written) *written = 0;
    if ((vm_address_t)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (!size) return 1;
    kr = vm_write( mach_task_self(), (vm_address_t)ptr, (vm_offset_t)(uintptr_t)src,
                   (mach_msg_type_number_t)size );
    set_kern_error( kr );
    if (kr != KERN_SUCCESS) return 0;
    if (written) *written = size;
    return 1;
}

#endif /* USE_IOS_THREADS */
