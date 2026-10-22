// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part C: the Mach exception port of the app's own task.
//
// The app's x64 interpreter runs a program's x64 code on its own emulator stack, where a signal
// frame can't become a Windows exception; it asks to see bad accesses first, with the faulting
// thread stopped. One receive port, installed once as the task's EXC_BAD_ACCESS port
// (EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES), and one server thread answer every such fault of
// every thread of the app:
//   - a registered Windows thread (Part D) gets the low-window service (clarification 1) and
//     then mid_fxi_mach_fault(); when either handled it, its state is written back and the reply
//     is KERN_SUCCESS: the thread goes on;
//   - every other case is answered KERN_FAILURE: the kernel then delivers the fault as a BSD
//     signal to the thread, and signal_arm64_ios.c (Part B) handles it.
// The server never calls Windows code, never allocates, takes no lock a faulting thread could
// hold and logs with write(2) only (ios_log_write). It touches no other task.
//
// The message layouts are those of the mach_exc MIG subsystem (Apple's <mach/mach_exc.defs>):
// mach_exception_raise is message 2405, its reply 2505.

#if 0
#pragma makedep unix
#endif

#if defined(__aarch64__) && defined(WINE_IOS)

#include <mach/mach.h>
#include <mach/vm_map.h>   /* vm_read_overwrite: <mach/mach_vm.h> is a stub in the iPhoneOS SDK */
#include <mach/exception_types.h>
#include <mach/ndr.h>
#include <mach/thread_info.h>
#include <mach/thread_status.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ios_sig_pure.h"
#include "ios_lowwin_fault.h"
#include "ios_mach_exc.h"

/* provided by the app (its x64 interpreter); non-zero: handled, the state was updated */
extern int mid_fxi_mach_fault( mach_port_t thread, arm_thread_state64_t *state, uint64_t fault_addr )
    __attribute__((weak_import));

extern void ios_log_write( const char *buf, int len );   /* debug.c: write(2) to the log fd */

#define EXC_RAISE_ID 2405               /* mach_exception_raise */
#define EXC_REPLY_ID (EXC_RAISE_ID + 100)

#pragma pack(push, 4)
struct exc_request
{
    mach_msg_header_t          head;
    /* the part the kernel fills in */
    mach_msg_body_t            body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    /* the rest */
    NDR_record_t               ndr;
    exception_type_t           exception;
    mach_msg_type_number_t     code_count;
    int64_t                    code[2];
};

struct exc_reply
{
    mach_msg_header_t head;
    NDR_record_t      ndr;
    kern_return_t     ret;
};
#pragma pack(pop)

/* the low-window service sees the thread state through the mirrors of ios_sig_pure.h */
_Static_assert( sizeof(struct ios_darwin_ss) == sizeof(arm_thread_state64_t), "thread state mirror" );
_Static_assert( offsetof( struct ios_darwin_ss, pc ) == offsetof( arm_thread_state64_t, __pc ), "pc" );
_Static_assert( sizeof(struct ios_darwin_ns) == sizeof(arm_neon_state64_t), "neon state mirror" );

static mach_port_t exc_port = MACH_PORT_NULL;


/* one line to ntdll's log: no locks, no allocation */
static void exc_log( const char *fmt, ... )
{
    char buffer[256];
    va_list args;
    size_t len;

    len = ios_sfmt( buffer, sizeof(buffer), "ntdll-ios: mach exceptions: " );
    va_start( args, fmt );
    len += ios_vsfmt( buffer + len, sizeof(buffer) - len, fmt, args );
    va_end( args );
    ios_log_write( buffer, (int)len );
}


/* Part D, exact match: by the port name the message carries, else by the thread's unique id
 * (the message may carry another name for the same thread than the one it registered) */
static uintptr_t registered_teb( mach_port_t thread )
{
    thread_identifier_info_data_t info;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    uintptr_t teb;

    if ((teb = ios_threadreg_teb_of( thread ))) return teb;
    if (thread_info( thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &count )) return 0;
    return ios_threadreg_teb_of_id( info.thread_id );
}


/* Clarification 1: services a low-window fault of a stopped Windows thread with the PEB of its
 * own registry entry's TEB (read with vm_read_overwrite, which fails instead of faulting). TRUE when the
 * thread's new state was written back. */
static int service_low_window( mach_port_t thread, uintptr_t teb, arm_thread_state64_t *state,
                               uint64_t fault_addr )
{
    struct ios_darwin_ss *ss = (struct ios_darwin_ss *)state;
    arm_neon_state64_t neon;
    mach_msg_type_number_t count;
    vm_size_t got = 0;
    uint64_t peb = 0;
    int ret;

    if (vm_read_overwrite( mach_task_self(), (vm_address_t)(teb + IOS_LW_TEB_PEB), sizeof(peb),
                           (vm_address_t)&peb, &got ) || got != sizeof(peb))
        return 0;

    ret = ios_lowwin_service( ss, NULL, fault_addr, (const void *)(uintptr_t)peb );
    if (ret == IOS_LW_NEED_NS)
    {
        count = ARM_NEON_STATE64_COUNT;
        if (thread_get_state( thread, ARM_NEON_STATE64, (thread_state_t)&neon, &count )) return 0;
        ret = ios_lowwin_service( ss, (struct ios_darwin_ns *)&neon, fault_addr, (const void *)(uintptr_t)peb );
        if (ret == IOS_LW_DONE &&
            thread_set_state( thread, ARM_NEON_STATE64, (thread_state_t)&neon, ARM_NEON_STATE64_COUNT ))
            return 0;
    }
    if (ret != IOS_LW_DONE) return 0;
    return !thread_set_state( thread, ARM_THREAD_STATE64, (thread_state_t)state, ARM_THREAD_STATE64_COUNT );
}


/* Part C2: one EXC_BAD_ACCESS of a thread */
static kern_return_t handle_bad_access( mach_port_t thread, uint64_t fault_addr )
{
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    uintptr_t teb;

    if (!(teb = registered_teb( thread ))) return KERN_FAILURE;   /* not a Windows thread */
    if (thread_get_state( thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count )) return KERN_FAILURE;

    /* clarification 1: a low window first (the interpreter's own loads fault this way too) */
    if (fault_addr < ((uint64_t)1 << 32) && service_low_window( thread, teb, &state, fault_addr ))
        return KERN_SUCCESS;

    if (!mid_fxi_mach_fault || !mid_fxi_mach_fault( thread, &state, fault_addr )) return KERN_FAILURE;
    if (thread_set_state( thread, ARM_THREAD_STATE64, (thread_state_t)&state, ARM_THREAD_STATE64_COUNT ))
    {
        exc_log( "thread_set_state failed after the interpreter handled a fault at %p\n", (void *)fault_addr );
        return KERN_FAILURE;
    }
    return KERN_SUCCESS;
}


/* Part C2/C3: answers one message; the port rights it brought are always given back */
static void handle_message( struct exc_request *req )
{
    struct exc_reply reply;
    kern_return_t ret = KERN_FAILURE;

    if (req->head.msgh_id != EXC_RAISE_ID ||
        !(req->head.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
        req->body.msgh_descriptor_count != 2 ||
        req->code_count > 2 ||
        req->head.msgh_size < offsetof( struct exc_request, code ) + req->code_count * sizeof(req->code[0]))
    {
        /* nothing this server knows: drop it with its rights (a reply right too, the sender
         * gets its send-once notification) */
        mach_msg_destroy( &req->head );
        return;
    }

    if (req->exception == EXC_BAD_ACCESS && req->code_count == 2)
        ret = handle_bad_access( req->thread.name, (uint64_t)req->code[1] );

    mach_port_deallocate( mach_task_self(), req->thread.name );
    mach_port_deallocate( mach_task_self(), req->task.name );

    memset( &reply, 0, sizeof(reply) );
    reply.head.msgh_bits        = MACH_MSGH_BITS( MACH_MSGH_BITS_REMOTE( req->head.msgh_bits ), 0 );
    reply.head.msgh_remote_port = req->head.msgh_remote_port;
    reply.head.msgh_local_port  = MACH_PORT_NULL;
    reply.head.msgh_size        = sizeof(reply);
    reply.head.msgh_id          = req->head.msgh_id + 100;
    reply.ndr                   = NDR_record;
    reply.ret                   = ret;
    if (mach_msg( &reply.head, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL,
                  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL ))
        exc_log( "reply failed\n" );
}


static void *exc_server( void *arg )
{
    union
    {
        struct exc_request req;
        char               buffer[1024];   /* any exception message with its trailer */
    } msg;
    sigset_t all;

    /* asynchronous signals go to other threads; this one only waits for messages */
    sigfillset( &all );
    pthread_sigmask( SIG_BLOCK, &all, NULL );
    pthread_setname_np( "ntdll-ios-mach-exc" );

    for (;;)
    {
        kern_return_t kr;

        memset( &msg.req.head, 0, sizeof(msg.req.head) );
        kr = mach_msg( &msg.req.head, MACH_RCV_MSG, 0, sizeof(msg), exc_port,
                       MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL );
        if (kr == MACH_RCV_INTERRUPTED) continue;
        if (kr != KERN_SUCCESS)
        {
            exc_log( "receive failed (%x)\n", (unsigned int)kr );
            continue;
        }
        handle_message( &msg.req );
    }
    return NULL;
}


/***********************************************************************
 *           ios_mach_exc_init
 *
 * Part C1: once, during process initialisation, before the first Windows thread runs: the port,
 * the server thread, then the task's exception port.
 */
void ios_mach_exc_init(void)
{
    mach_port_t port;
    pthread_attr_t attr;
    pthread_t thread;
    kern_return_t kr;

    if (exc_port != MACH_PORT_NULL) return;
    if ((kr = mach_port_allocate( mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port )))
    {
        exc_log( "no port (%x): the interpreter won't see its faults first\n", (unsigned int)kr );
        return;
    }
    if ((kr = mach_port_insert_right( mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND )))
    {
        exc_log( "no send right (%x)\n", (unsigned int)kr );
        mach_port_mod_refs( mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1 );
        return;
    }
    exc_port = port;

    pthread_attr_init( &attr );
    pthread_attr_set_qos_class_np( &attr, QOS_CLASS_USER_INTERACTIVE, 0 );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    if (pthread_create( &thread, &attr, exc_server, NULL ))
    {
        pthread_attr_destroy( &attr );
        exc_log( "no server thread\n" );
        return;  /* the port stays unused: no exception is ever sent to it */
    }
    pthread_attr_destroy( &attr );

    if ((kr = task_set_exception_ports( mach_task_self(), EXC_MASK_BAD_ACCESS, port,
                                        EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64 )))
        exc_log( "task_set_exception_ports failed (%x)\n", (unsigned int)kr );
}

#endif /* __aarch64__ && WINE_IOS */
