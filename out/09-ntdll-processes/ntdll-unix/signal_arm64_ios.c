/*
 * ARM64 signal handling routines
 *
 * Copyright 2010-2013 André Hentschel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Changed for MYIOSDECK, 2026: this file is Wine 11.4's dlls/ntdll/unix/signal_arm64.c ported
 * to Darwin arm64 (iOS), where Windows processes run as threads of one app (specs 09 and 11).
 * It is compiled instead of signal_arm64.c when WINE_IOS is defined. The main differences:
 *   - x18 belongs to the platform. Nothing reads, saves or restores it: the TEB comes from the
 *     TSD slot of spec 09's key (TPIDRRO_EL0 + ios_teb_tls_slot_offset), in C and in assembly.
 *   - Darwin's mcontext (__ss, __ns, __es) and assembler syntax; the dispatchers are upstream's
 *     with those two changes.
 *   - Signals run on a per-thread alternate stack; a thread with no TEB is not a Windows thread
 *     and its faults go back to the app's own handling; unix-side faults during a system call
 *     return their exception code as the call's status (logged, rate-limited); a signal storm,
 *     a fast fail or SIGABRT on a Windows thread ends only the pseudo-process.
 *   - No SIGUSR1/SIGUSR2/SIGINT/SIGQUIT handlers. The dispatcher's full-context return (upstream:
 *     SIGUSR2) goes through a SIGTRAP the thread sends itself.
 *   - A Mach exception server (ios_mach_exc.c) sees EXC_BAD_ACCESS first, for the app's x64
 *     interpreter and for the low windows of spec 10; a thread registry tells it which threads
 *     are Windows threads.
 * out/09-ntdll-processes/CHANGES.md lists the changes by Part ("spec 11").
 */

#if 0
#pragma makedep unix
#endif

#if defined(__aarch64__) && defined(WINE_IOS)

#include "config.h"

#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/vm_map.h>   /* vm_read_overwrite: <mach/mach_vm.h> is a stub in the iPhoneOS SDK */
#include <mach/thread_status.h>
#include <mach-o/dyld.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/asm.h"
#include "unix_private.h"
#include "wine/debug.h"
#include "ios_syscall_frame.h"
#include "ios_sig_pure.h"
#include "ios_dylib.h"
#include "ios_mach_exc.h"
#include "ios_lowwin_fault.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);

#define NTDLL_DWARF_H_NO_UNWINDER
#include "dwarf.h"

/***********************************************************************
 * signal context platform-specific definitions (Darwin arm64)
 */

/* All Registers access - only for local access */
#define REG_sig(reg_name, context) ((context)->uc_mcontext->__ss.__ ## reg_name)
#define REGn_sig(reg_num, context) ((context)->uc_mcontext->__ss.__x[reg_num])

/* Special Registers access  */
#define SP_sig(context)            REG_sig(sp, context)    /* Stack pointer */
#define PC_sig(context)            REG_sig(pc, context)    /* Program counter */
#define PSTATE_sig(context)        REG_sig(cpsr, context)  /* Current State Register */
#define FP_sig(context)            REG_sig(fp, context)    /* Frame pointer */
#define LR_sig(context)            REG_sig(lr, context)    /* Link Register */

/* MYIOSDECK: the pure conversions (ios_sig_pure.c) work on mirrors of these structures */
#define SS_sig(context) ((struct ios_darwin_ss *)&(context)->uc_mcontext->__ss)
#define NS_sig(context) ((struct ios_darwin_ns *)&(context)->uc_mcontext->__ns)

static DWORD64 get_fault_esr( ucontext_t *sigcontext )
{
    return sigcontext->uc_mcontext->__es.__esr;
}

/* MYIOSDECK (spec 11 Part A5): the mirrors in ios_sig_pure.h have the layouts of the real
 * structures */
C_ASSERT( sizeof(struct ios_darwin_ss) == sizeof(arm_thread_state64_t) );
C_ASSERT( offsetof( struct ios_darwin_ss, x ) == offsetof( arm_thread_state64_t, __x ) );
C_ASSERT( offsetof( struct ios_darwin_ss, fp ) == offsetof( arm_thread_state64_t, __fp ) );
C_ASSERT( offsetof( struct ios_darwin_ss, lr ) == offsetof( arm_thread_state64_t, __lr ) );
C_ASSERT( offsetof( struct ios_darwin_ss, sp ) == offsetof( arm_thread_state64_t, __sp ) );
C_ASSERT( offsetof( struct ios_darwin_ss, pc ) == offsetof( arm_thread_state64_t, __pc ) );
C_ASSERT( offsetof( struct ios_darwin_ss, cpsr ) == offsetof( arm_thread_state64_t, __cpsr ) );
C_ASSERT( sizeof(struct ios_darwin_ns) == sizeof(arm_neon_state64_t) );
C_ASSERT( offsetof( struct ios_darwin_ns, v ) == offsetof( arm_neon_state64_t, __v ) );
C_ASSERT( offsetof( struct ios_darwin_ns, fpsr ) == offsetof( arm_neon_state64_t, __fpsr ) );
C_ASSERT( offsetof( struct ios_darwin_ns, fpcr ) == offsetof( arm_neon_state64_t, __fpcr ) );
C_ASSERT( sizeof(struct ios_win_ctx) == sizeof(CONTEXT) );
C_ASSERT( offsetof( struct ios_win_ctx, ContextFlags ) == offsetof( CONTEXT, ContextFlags ) );
C_ASSERT( offsetof( struct ios_win_ctx, Cpsr ) == offsetof( CONTEXT, Cpsr ) );
C_ASSERT( offsetof( struct ios_win_ctx, X ) == offsetof( CONTEXT, X0 ) );
C_ASSERT( offsetof( struct ios_win_ctx, Fp ) == offsetof( CONTEXT, Fp ) );
C_ASSERT( offsetof( struct ios_win_ctx, Lr ) == offsetof( CONTEXT, Lr ) );
C_ASSERT( offsetof( struct ios_win_ctx, Sp ) == offsetof( CONTEXT, Sp ) );
C_ASSERT( offsetof( struct ios_win_ctx, Pc ) == offsetof( CONTEXT, Pc ) );
C_ASSERT( offsetof( struct ios_win_ctx, V ) == offsetof( CONTEXT, V ) );
C_ASSERT( offsetof( struct ios_win_ctx, Fpcr ) == offsetof( CONTEXT, Fpcr ) );
C_ASSERT( offsetof( struct ios_win_ctx, Fpsr ) == offsetof( CONTEXT, Fpsr ) );
C_ASSERT( offsetof( struct ios_win_ctx, Bcr ) == offsetof( CONTEXT, Bcr ) );
C_ASSERT( offsetof( struct ios_win_ctx, Bvr ) == offsetof( CONTEXT, Bvr ) );
C_ASSERT( offsetof( struct ios_win_ctx, Wcr ) == offsetof( CONTEXT, Wcr ) );
C_ASSERT( offsetof( struct ios_win_ctx, Wvr ) == offsetof( CONTEXT, Wvr ) );

/* MYIOSDECK (spec 11 Part A2): the TEB offsets the assembly below and spec 08 use */
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct ntdll_thread_data, syscall_table ) == IOS_TEB_SYSCALL_TABLE );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct ntdll_thread_data, syscall_frame ) == IOS_TEB_SYSCALL_FRAME );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct ntdll_thread_data, syscall_trace ) == IOS_TEB_SYSCALL_TRACE );
C_ASSERT( offsetof( TEB, GdiTebBatch ) + offsetof( struct ntdll_thread_data, kernel_stack ) == IOS_TEB_KERNEL_STACK );
C_ASSERT( offsetof( TEB, Peb ) == IOS_TEB_PEB );
C_ASSERT( offsetof( TEB, Peb ) == IOS_LW_TEB_PEB );
C_ASSERT( offsetof( TEB, Tib.ExceptionList ) == 0 );

/* MYIOSDECK: what this file keeps per thread, in the CPU-specific part of the thread data
 * (upstream's arm64 code doesn't use it) */
struct arm64_thread_data
{
    void             *altstack;       /* the alternate signal stack's mapping (Part B) */
    size_t            altstack_size;  /* its size, guard page included */
    struct ios_storm  storm;          /* identical faults in a row (Part B4) */
    unsigned int      mach_port;      /* the registered Mach thread port, 0 if none (Part D) */
    int               full_restore;   /* the dispatcher asked for a full-context return */
};

C_ASSERT( sizeof(struct arm64_thread_data) <= sizeof(((struct ntdll_thread_data *)0)->cpu_data) );

static inline struct arm64_thread_data *arm64_thread_data_of( TEB *teb )
{
    return (struct arm64_thread_data *)((struct ntdll_thread_data *)&teb->GdiTebBatch)->cpu_data;
}

static inline struct arm64_thread_data *arm64_thread_data(void)
{
    return arm64_thread_data_of( NtCurrentTeb() );
}

/* a new thread's set-up before it runs Windows code (Parts B and D) */
static void ios_signal_thread_start( TEB *teb );

/* stack layout when calling KiUserExceptionDispatcher */
struct exc_stack_layout
{
    CONTEXT              context;        /* 000 */
    CONTEXT_EX           context_ex;     /* 390 */
    EXCEPTION_RECORD     rec;            /* 3b0 */
    ULONG64              align;          /* 448 */
    ULONG64              redzone[2];     /* 450 */
};
C_ASSERT( offsetof(struct exc_stack_layout, rec) == 0x3b0 );
C_ASSERT( sizeof(struct exc_stack_layout) == 0x460 );

/* stack layout when calling KiUserApcDispatcher */
struct apc_stack_layout
{
    void                *func;           /* 000 APC to call*/
    ULONG64              args[3];        /* 008 function arguments */
    ULONG64              alertable;      /* 020 */
    ULONG64              align;          /* 028 */
    CONTEXT              context;        /* 030 */
    ULONG64              redzone[2];     /* 3c0 */
};
C_ASSERT( offsetof(struct apc_stack_layout, context) == 0x30 );
C_ASSERT( sizeof(struct apc_stack_layout) == 0x3d0 );

/* stack layout when calling KiUserCallbackDispatcher */
struct callback_stack_layout
{
    void                *args;           /* 000 arguments */
    ULONG                len;            /* 008 arguments len */
    ULONG                id;             /* 00c function id */
    ULONG64              unknown;        /* 010 */
    ULONG64              lr;             /* 018 */
    ULONG64              sp;             /* 020 sp+pc (machine frame) */
    ULONG64              pc;             /* 028 */
    BYTE                 args_data[0];   /* 030 copied argument data*/
};
C_ASSERT( offsetof(struct callback_stack_layout, sp) == 0x20 );
C_ASSERT( sizeof(struct callback_stack_layout) == 0x30 );

/* MYIOSDECK: struct syscall_frame is in ios_syscall_frame.h (shared with spec 08) */


/***********************************************************************
 *           context_init_empty_xstate
 *
 * Initializes a context's CONTEXT_EX structure to point to an empty xstate buffer
 */
static inline void context_init_empty_xstate( CONTEXT *context, void *xstate_buffer )
{
    CONTEXT_EX *xctx;

    xctx = (CONTEXT_EX *)(context + 1);
    xctx->Legacy.Length = sizeof(CONTEXT);
    xctx->Legacy.Offset = -(LONG)sizeof(CONTEXT);
    xctx->XState.Length = 0;
    xctx->XState.Offset = (BYTE *)xstate_buffer - (BYTE *)xctx;
    xctx->All.Length = sizeof(CONTEXT) + xctx->XState.Offset + xctx->XState.Length;
    xctx->All.Offset = -(LONG)sizeof(CONTEXT);
}

void set_process_instrumentation_callback( void *callback )
{
    if (callback) FIXME( "Not supported.\n" );
}


/***********************************************************************
 *           syscall_frame_fixup_for_fastpath
 *
 * Fixes up the given syscall frame such that the syscall dispatcher
 * can return via the fast path if CONTEXT_INTEGER is set in
 * restore_flags.
 *
 * Clobbers the frame's X16 and X17 register values.
 */
static void syscall_frame_fixup_for_fastpath( struct syscall_frame *frame )
{
    frame->x[16] = frame->pc;
    frame->x[17] = frame->sp;
}


/***********************************************************************
 *           save_context
 *
 * Set the register values from a sigcontext.
 * MYIOSDECK (spec 11 Part A5): Darwin's __ss and __ns, through the tested conversion; X18 is
 * the TEB, whatever the platform left in x18.
 */
static void save_context( CONTEXT *context, const ucontext_t *sigcontext )
{
    ios_ctx_from_darwin( (struct ios_win_ctx *)context,
                         (const struct ios_darwin_ss *)&sigcontext->uc_mcontext->__ss,
                         (const struct ios_darwin_ns *)&sigcontext->uc_mcontext->__ns,
                         (ULONG_PTR)NtCurrentTeb() );
    context->ContextFlags = CONTEXT_FULL;
}


/***********************************************************************
 *           restore_context
 *
 * Build a sigcontext from the register values.
 * MYIOSDECK: x18 is never written.
 */
static void restore_context( const CONTEXT *context, ucontext_t *sigcontext )
{
    ios_ctx_to_darwin( (const struct ios_win_ctx *)context, SS_sig(sigcontext), NS_sig(sigcontext) );
}


/***********************************************************************
 *           signal_set_full_context
 */
NTSTATUS signal_set_full_context( CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (!status && (context->ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
        frame->restore_flags |= CONTEXT_INTEGER;

    if (is_arm64ec() && !is_ec_code( frame->pc ))
    {
        CONTEXT *user_context = (CONTEXT *)((frame->sp - sizeof(CONTEXT)) & ~15);

        user_context->ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), user_context );
        frame->sp = (ULONG_PTR)user_context;
        frame->pc = (ULONG_PTR)pKiUserEmulationDispatcher;
    }
    return status;
}


/***********************************************************************
 *              get_native_context
 */
void *get_native_context( CONTEXT *context )
{
    return context;
}


/***********************************************************************
 *              get_wow_context
 */
void *get_wow_context( CONTEXT *context )
{
    return get_cpu_area( main_image_info.Machine );
}


/***********************************************************************
 *              NtSetContextThread  (NTDLL.@)
 *              ZwSetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    NTSTATUS ret = STATUS_SUCCESS;
    BOOL self = (handle == GetCurrentThread());
    DWORD flags = context->ContextFlags & ~CONTEXT_ARM64;

    if (self && (flags & CONTEXT_DEBUG_REGISTERS)) self = FALSE;

    if (!self)
    {
        ret = set_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );
        if (ret || !self) return ret;
    }

    if (flags & CONTEXT_INTEGER)
    {
        memcpy( frame->x, context->X, sizeof(context->X[0]) * 18 );
        /* skip x18 */
        memcpy( frame->x + 19, context->X + 19, sizeof(context->X[0]) * 10 );
    }
    if (flags & CONTEXT_CONTROL)
    {
        frame->fp    = context->Fp;
        frame->lr    = context->Lr;
        frame->sp    = context->Sp;
        frame->pc    = context->Pc;
        frame->cpsr  = context->Cpsr;
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        frame->fpcr = context->Fpcr;
        frame->fpsr = context->Fpsr;
        memcpy( frame->v, context->V, sizeof(frame->v) );
    }
    /* MYIOSDECK: CONTEXT_ARM64_X18 is ignored; the platform owns x18 and the dispatcher never
     * restores it */
    if (flags & CONTEXT_DEBUG_REGISTERS) FIXME( "debug registers not supported\n" );
    frame->restore_flags |= flags & ~CONTEXT_INTEGER;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtGetContextThread  (NTDLL.@)
 *              ZwGetContextThread  (NTDLL.@)
 *
 * MYIOSDECK (spec 11 Part A5): another thread's context comes from the server (spec 08 Part E,
 * through get_thread_context); the own thread's from its syscall frame, as upstream.
 */
NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    DWORD needed_flags = context->ContextFlags & ~CONTEXT_ARM64;
    BOOL self = (handle == GetCurrentThread());

    if (!self)
    {
        NTSTATUS ret = get_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );
        if (ret || !self) return ret;
    }

    if (needed_flags & CONTEXT_INTEGER)
    {
        memcpy( context->X, frame->x, sizeof(context->X[0]) * 29 );
        context->X[18] = (ULONG_PTR)NtCurrentTeb();  /* MYIOSDECK: x18 isn't saved; Windows has the TEB there */
        context->ContextFlags |= CONTEXT_INTEGER;
    }
    if (needed_flags & CONTEXT_CONTROL)
    {
        context->Fp   = frame->fp;
        context->Lr   = frame->lr;
        context->Sp   = frame->sp;
        context->Pc   = frame->pc;
        context->Cpsr = frame->cpsr;
        context->ContextFlags |= CONTEXT_CONTROL;
    }
    if (needed_flags & CONTEXT_FLOATING_POINT)
    {
        context->Fpcr = frame->fpcr;
        context->Fpsr = frame->fpsr;
        memcpy( context->V, frame->v, sizeof(context->V) );
        context->ContextFlags |= CONTEXT_FLOATING_POINT;
    }
    if (needed_flags & CONTEXT_DEBUG_REGISTERS) FIXME( "debug registers not supported\n" );
    set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              set_thread_wow64_context
 */
NTSTATUS set_thread_wow64_context( HANDLE handle, const void *ctx, ULONG size )
{
    BOOL self = (handle == GetCurrentThread());
    USHORT machine;
    void *frame;

    switch (size)
    {
    case sizeof(I386_CONTEXT): machine = IMAGE_FILE_MACHINE_I386; break;
    case sizeof(ARM_CONTEXT): machine = IMAGE_FILE_MACHINE_ARMNT; break;
    default: return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (!self)
    {
        NTSTATUS ret = set_thread_context( handle, ctx, &self, machine );
        if (ret || !self) return ret;
    }

    if (!(frame = get_cpu_area( machine ))) return STATUS_INVALID_PARAMETER;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
    {
        I386_CONTEXT *wow_frame = frame;
        const I386_CONTEXT *context = ctx;
        DWORD flags = context->ContextFlags & ~CONTEXT_i386;

        if (flags & CONTEXT_I386_INTEGER)
        {
            wow_frame->Eax = context->Eax;
            wow_frame->Ebx = context->Ebx;
            wow_frame->Ecx = context->Ecx;
            wow_frame->Edx = context->Edx;
            wow_frame->Esi = context->Esi;
            wow_frame->Edi = context->Edi;
        }
        if (flags & CONTEXT_I386_CONTROL)
        {
            WOW64_CPURESERVED *cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];

            wow_frame->Esp    = context->Esp;
            wow_frame->Ebp    = context->Ebp;
            wow_frame->Eip    = context->Eip;
            wow_frame->EFlags = context->EFlags;
            wow_frame->SegCs  = context->SegCs;
            wow_frame->SegSs  = context->SegSs;
            cpu->Flags |= WOW64_CPURESERVED_FLAG_RESET_STATE;
        }
        if (flags & CONTEXT_I386_SEGMENTS)
        {
            wow_frame->SegDs = context->SegDs;
            wow_frame->SegEs = context->SegEs;
            wow_frame->SegFs = context->SegFs;
            wow_frame->SegGs = context->SegGs;
        }
        if (flags & CONTEXT_I386_DEBUG_REGISTERS)
        {
            wow_frame->Dr0 = context->Dr0;
            wow_frame->Dr1 = context->Dr1;
            wow_frame->Dr2 = context->Dr2;
            wow_frame->Dr3 = context->Dr3;
            wow_frame->Dr6 = context->Dr6;
            wow_frame->Dr7 = context->Dr7;
        }
        if (flags & CONTEXT_I386_EXTENDED_REGISTERS)
        {
            memcpy( &wow_frame->ExtendedRegisters, context->ExtendedRegisters, sizeof(context->ExtendedRegisters) );
        }
        if (flags & CONTEXT_I386_FLOATING_POINT)
        {
            memcpy( &wow_frame->FloatSave, &context->FloatSave, sizeof(context->FloatSave) );
        }
        /* FIXME: CONTEXT_I386_XSTATE */
        break;
    }

    case IMAGE_FILE_MACHINE_ARMNT:
    {
        ARM_CONTEXT *wow_frame = frame;
        const ARM_CONTEXT *context = ctx;
        DWORD flags = context->ContextFlags & ~CONTEXT_ARM;

        if (flags & CONTEXT_INTEGER)
        {
            wow_frame->R0  = context->R0;
            wow_frame->R1  = context->R1;
            wow_frame->R2  = context->R2;
            wow_frame->R3  = context->R3;
            wow_frame->R4  = context->R4;
            wow_frame->R5  = context->R5;
            wow_frame->R6  = context->R6;
            wow_frame->R7  = context->R7;
            wow_frame->R8  = context->R8;
            wow_frame->R9  = context->R9;
            wow_frame->R10 = context->R10;
            wow_frame->R11 = context->R11;
            wow_frame->R12 = context->R12;
        }
        if (flags & CONTEXT_CONTROL)
        {
            wow_frame->Sp = context->Sp;
            wow_frame->Lr = context->Lr;
            wow_frame->Pc = context->Pc & ~1;
            wow_frame->Cpsr = context->Cpsr;
            if (context->Cpsr & 0x20) wow_frame->Pc |= 1; /* thumb */
        }
        if (flags & CONTEXT_FLOATING_POINT)
        {
            wow_frame->Fpscr = context->Fpscr;
            memcpy( wow_frame->D, context->D, sizeof(context->D) );
        }
        break;
    }

    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              get_thread_wow64_context
 */
NTSTATUS get_thread_wow64_context( HANDLE handle, void *ctx, ULONG size )
{
    BOOL self = (handle == GetCurrentThread());
    USHORT machine;
    void *frame;

    switch (size)
    {
    case sizeof(I386_CONTEXT): machine = IMAGE_FILE_MACHINE_I386; break;
    case sizeof(ARM_CONTEXT): machine = IMAGE_FILE_MACHINE_ARMNT; break;
    default: return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (!self)
    {
        NTSTATUS ret = get_thread_context( handle, ctx, &self, machine );
        if (ret || !self) return ret;
    }

    if (!(frame = get_cpu_area( machine ))) return STATUS_INVALID_PARAMETER;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
    {
        I386_CONTEXT *wow_frame = frame, *context = ctx;
        DWORD needed_flags = context->ContextFlags & ~CONTEXT_i386;

        if (needed_flags & CONTEXT_I386_INTEGER)
        {
            context->Eax = wow_frame->Eax;
            context->Ebx = wow_frame->Ebx;
            context->Ecx = wow_frame->Ecx;
            context->Edx = wow_frame->Edx;
            context->Esi = wow_frame->Esi;
            context->Edi = wow_frame->Edi;
            context->ContextFlags |= CONTEXT_I386_INTEGER;
        }
        if (needed_flags & CONTEXT_I386_CONTROL)
        {
            context->Esp    = wow_frame->Esp;
            context->Ebp    = wow_frame->Ebp;
            context->Eip    = wow_frame->Eip;
            context->EFlags = wow_frame->EFlags;
            context->SegCs  = wow_frame->SegCs;
            context->SegSs  = wow_frame->SegSs;
            context->ContextFlags |= CONTEXT_I386_CONTROL;
        }
        if (needed_flags & CONTEXT_I386_SEGMENTS)
        {
            context->SegDs = wow_frame->SegDs;
            context->SegEs = wow_frame->SegEs;
            context->SegFs = wow_frame->SegFs;
            context->SegGs = wow_frame->SegGs;
            context->ContextFlags |= CONTEXT_I386_SEGMENTS;
        }
        if (needed_flags & CONTEXT_I386_EXTENDED_REGISTERS)
        {
            memcpy( context->ExtendedRegisters, &wow_frame->ExtendedRegisters, sizeof(context->ExtendedRegisters) );
            context->ContextFlags |= CONTEXT_I386_EXTENDED_REGISTERS;
        }
        if (needed_flags & CONTEXT_I386_FLOATING_POINT)
        {
            memcpy( &context->FloatSave, &wow_frame->FloatSave, sizeof(context->FloatSave) );
            context->ContextFlags |= CONTEXT_I386_FLOATING_POINT;
        }
        if (needed_flags & CONTEXT_I386_DEBUG_REGISTERS)
        {
            context->Dr0 = wow_frame->Dr0;
            context->Dr1 = wow_frame->Dr1;
            context->Dr2 = wow_frame->Dr2;
            context->Dr3 = wow_frame->Dr3;
            context->Dr6 = wow_frame->Dr6;
            context->Dr7 = wow_frame->Dr7;
        }
        /* FIXME: CONTEXT_I386_XSTATE */
        set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
        break;
    }

    case IMAGE_FILE_MACHINE_ARMNT:
    {
        ARM_CONTEXT *wow_frame = frame, *context = ctx;
        DWORD needed_flags = context->ContextFlags & ~CONTEXT_ARM;

        if (needed_flags & CONTEXT_INTEGER)
        {
            context->R0  = wow_frame->R0;
            context->R1  = wow_frame->R1;
            context->R2  = wow_frame->R2;
            context->R3  = wow_frame->R3;
            context->R4  = wow_frame->R4;
            context->R5  = wow_frame->R5;
            context->R6  = wow_frame->R6;
            context->R7  = wow_frame->R7;
            context->R8  = wow_frame->R8;
            context->R9  = wow_frame->R9;
            context->R10 = wow_frame->R10;
            context->R11 = wow_frame->R11;
            context->R12 = wow_frame->R12;
            context->ContextFlags |= CONTEXT_INTEGER;
        }
        if (needed_flags & CONTEXT_CONTROL)
        {
            context->Sp   = wow_frame->Sp;
            context->Lr   = wow_frame->Lr;
            context->Pc   = wow_frame->Pc;
            context->Cpsr = wow_frame->Cpsr;
            context->ContextFlags |= CONTEXT_CONTROL;
        }
        if (needed_flags & CONTEXT_FLOATING_POINT)
        {
            context->Fpscr = wow_frame->Fpscr;
            memcpy( context->D, wow_frame->D, sizeof(wow_frame->D) );
            context->ContextFlags |= CONTEXT_FLOATING_POINT;
        }
        set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
        break;
    }

    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           call_user_apc_dispatcher
 */
NTSTATUS call_user_apc_dispatcher( CONTEXT *context, unsigned int flags, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                   PNTAPCFUNC func, NTSTATUS status )
{
    struct syscall_frame *frame = get_syscall_frame();
    ULONG64 sp = context ? context->Sp : frame->sp;
    struct apc_stack_layout *stack;

    if (flags) FIXME( "flags %#x are not supported.\n", flags );

    sp &= ~15;
    stack = (struct apc_stack_layout *)sp - 1;
    if (context)
    {
        memmove( &stack->context, context, sizeof(stack->context) );
        NtSetContextThread( GetCurrentThread(), &stack->context );
    }
    else
    {
        stack->context.ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), &stack->context );
        stack->context.X0 = status;
    }
    stack->func      = func;
    stack->args[0]   = arg1;
    stack->args[1]   = arg2;
    stack->args[2]   = arg3;
    stack->alertable = TRUE;

    frame->sp = (ULONG64)stack;
    frame->pc = (ULONG64)pKiUserApcDispatcher;
    frame->restore_flags |= CONTEXT_CONTROL;
    syscall_frame_fixup_for_fastpath( frame );
    return status;
}


/***********************************************************************
 *           call_raise_user_exception_dispatcher
 */
void call_raise_user_exception_dispatcher(void)
{
    get_syscall_frame()->pc = (UINT64)pKiRaiseUserExceptionDispatcher;
}


/***********************************************************************
 *           call_user_exception_dispatcher
 */
NTSTATUS call_user_exception_dispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    struct exc_stack_layout *stack;
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (status) return status;
    stack = (struct exc_stack_layout *)(context->Sp & ~15) - 1;
    memmove( &stack->context, context, sizeof(*context) );
    memmove( &stack->rec, rec, sizeof(*rec) );
    context_init_empty_xstate( &stack->context, stack->redzone );

    frame->pc = (ULONG64)pKiUserExceptionDispatcher;
    frame->sp = (ULONG64)stack;
    frame->restore_flags |= CONTEXT_CONTROL;
    syscall_frame_fixup_for_fastpath( frame );
    return status;
}


/***********************************************************************
 * MYIOSDECK (spec 11 Part A1): the calling thread's TEB, in assembly. Darwin's pthread TSD array
 * is at TPIDRRO_EL0 (its low three bits may hold the CPU number); spec 09's key indexes it at
 * ios_teb_tls_slot_offset. reg gets the TEB, tmp is clobbered; x18 is never used.
 */
#define IOS_ASM_LOAD_TEB(reg, tmp) \
    "mrs " reg ", tpidrro_el0\n\t" \
    "and " reg ", " reg ", #0xfffffffffffffff8\n\t" \
    "adrp " tmp ", " __ASM_NAME("ios_teb_tls_slot_offset") "@PAGE\n\t" \
    "ldrsw " tmp ", [" tmp ", " __ASM_NAME("ios_teb_tls_slot_offset") "@PAGEOFF]\n\t" \
    "ldr " reg ", [" reg ", " tmp "]\n\t"


/***********************************************************************
 *           call_user_mode_callback
 *
 * MYIOSDECK: the TEB stays in x4 (upstream moves it to x18). The saved FPCR/FPSR pair is FPCR
 * in the low half and FPSR in the high half, as user_mode_callback_return reads it.
 */
extern NTSTATUS call_user_mode_callback( ULONG64 user_sp, void **ret_ptr, ULONG *ret_len,
                                         void *func, TEB *teb );
__ASM_GLOBAL_FUNC( call_user_mode_callback,
                   "stp x29, x30, [sp,#-0xd0]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0xd0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xd0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xc8\n\t")
                   "mov x29, sp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   "stp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   "stp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   "stp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   "stp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   "stp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "stp d8,  d9,  [x29, #0x60]\n\t"
                   "stp d10, d11, [x29, #0x70]\n\t"
                   "stp d12, d13, [x29, #0x80]\n\t"
                   "stp d14, d15, [x29, #0x90]\n\t"
                   "stp x1, x2, [x29, #0xa0]\n\t" /* ret_ptr, ret_len */
                   "mrs x1, fpcr\n\t"
                   "mrs x2, fpsr\n\t"
                   "bfi x1, x2, #32, #32\n\t"     /* fpcr | fpsr << 32 */
                   "ldr x2, [x4]\n\t"             /* teb->Tib.ExceptionList */
                   "stp x1, x2, [x29, #0xb0]\n\t"

                   "ldr x7, [x4, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "sub x1, sp, #0x330\n\t"       /* sizeof(struct syscall_frame) */
                   "str x1, [x4, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "add x8, x29, #0xd0\n\t"
                   "stp x7, x8, [x1, #0x110]\n\t" /* frame->prev_frame,syscall_cfa */
                   "ldr w11, [x4, #0x380]\n\t"    /* thread_data->syscall_trace */
                   "cbnz x11, 1f\n\t"
                   /* switch to user stack */
                   "mov sp, x0\n\t"               /* user_sp */
                   "br x3\n"
                   "1:\tmov x20, x0\n\t"          /* user_sp */
                   "mov x21, x3\n\t"              /* func */
                   "mov sp, x1\n\t"
                   "ldr x1, [x20]\n\t"            /* args */
                   "ldp w2, w0, [x20, #8]\n\t"    /* len, id */
                   "str x0, [x29, #0xc0]\n\t"     /* id */
                   "bl " __ASM_NAME("trace_usercall") "\n\t"
                   "mov sp, x20\n\t"              /* user_sp */
                   "br x21" )


/***********************************************************************
 *           user_mode_callback_return
 */
extern void DECLSPEC_NORETURN user_mode_callback_return( void *ret_ptr, ULONG ret_len,
                                                         NTSTATUS status, TEB *teb );
__ASM_GLOBAL_FUNC( user_mode_callback_return,
                   "ldr x4, [x3, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "ldp x5, x29, [x4,#0x110]\n\t" /* prev_frame,syscall_cfa */
                   "str x5, [x3, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "sub x29, x29, #0xd0\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   __ASM_CFI(".cfi_rel_offset 29,0x00\n\t")
                   __ASM_CFI(".cfi_rel_offset 30,0x08\n\t")
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "ldp x5, x6, [x29, #0xb0]\n\t"
                   "str x6, [x3]\n\t"             /* teb->Tib.ExceptionList */
                   "mov w7, w5\n\t"               /* fpcr */
                   "lsr x5, x5, #32\n\t"          /* fpsr */
                   "msr fpcr, x7\n\t"
                   "msr fpsr, x5\n\t"
                   "ldp x5, x6, [x29, #0xa0]\n\t" /* ret_ptr, ret_len */
                   "str x0, [x5]\n\t"             /* ret_ptr */
                   "str w1, [x6]\n\t"             /* ret_len */
                   "ldr w11, [x3, #0x380]\n\t"    /* thread_data->syscall_trace */
                   "cbz x11, 1f\n\t"
                   "ldr w3, [x29, #0xc0]\n\t"     /* id */
                   "mov x19, x2\n\t"
                   "bl " __ASM_NAME("trace_userret") "\n\t"
                   "mov x2, x19\n"                /* status */
                   "1:\tldp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_same_value 19\n\t")
                   __ASM_CFI(".cfi_same_value 20\n\t")
                   "ldp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_same_value 21\n\t")
                   __ASM_CFI(".cfi_same_value 22\n\t")
                   "ldp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_same_value 23\n\t")
                   __ASM_CFI(".cfi_same_value 24\n\t")
                   "ldp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_same_value 25\n\t")
                   __ASM_CFI(".cfi_same_value 26\n\t")
                   "ldp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_same_value 27\n\t")
                   __ASM_CFI(".cfi_same_value 28\n\t")
                   "ldp d8,  d9,  [x29, #0x60]\n\t"
                   "ldp d10, d11, [x29, #0x70]\n\t"
                   "ldp d12, d13, [x29, #0x80]\n\t"
                   "ldp d14, d15, [x29, #0x90]\n\t"
                   "mov x0, x2\n\t"               /* status */
                   "mov sp, x29\n\t"
                   "ldp x29, x30, [sp], #0xd0\n\t"
                   "ret" )


/***********************************************************************
 *           user_mode_abort_thread
 */
extern void DECLSPEC_NORETURN user_mode_abort_thread( NTSTATUS status, struct syscall_frame *frame );
__ASM_GLOBAL_FUNC( user_mode_abort_thread,
                   "ldr x1, [x1, #0x118]\n\t"    /* frame->syscall_cfa */
                   "sub x29, x1, #0xc0\n\t"
                   /* switch to kernel stack */
                   "mov sp, x29\n\t"
                   __ASM_CFI(".cfi_def_cfa 29,0xc0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19,-0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20,-0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21,-0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22,-0x98\n\t")
                   __ASM_CFI(".cfi_offset 23,-0x90\n\t")
                   __ASM_CFI(".cfi_offset 24,-0x88\n\t")
                   __ASM_CFI(".cfi_offset 25,-0x80\n\t")
                   __ASM_CFI(".cfi_offset 26,-0x78\n\t")
                   __ASM_CFI(".cfi_offset 27,-0x70\n\t")
                   __ASM_CFI(".cfi_offset 28,-0x68\n\t")
                   "bl " __ASM_NAME("abort_thread") )


/***********************************************************************
 *           KeUserModeCallback
 */
NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    struct syscall_frame *frame = get_syscall_frame();
    ULONG64 sp = (frame->sp - offsetof( struct callback_stack_layout, args_data[len] ) - 16) & ~15;
    struct callback_stack_layout *stack = (struct callback_stack_layout *)sp;

    if ((char *)ntdll_get_thread_data()->kernel_stack + min_kernel_stack > (char *)&frame)
        return STATUS_STACK_OVERFLOW;

    stack->args = stack->args_data;
    stack->len  = len;
    stack->id   = id;
    stack->lr   = frame->lr;
    stack->sp   = frame->sp;
    stack->pc   = frame->pc;
    memcpy( stack->args_data, args, len );
    return call_user_mode_callback( sp, ret_ptr, ret_len, pKiUserCallbackDispatcher, NtCurrentTeb() );
}


/***********************************************************************
 *           NtCallbackReturn  (NTDLL.@)
 */
NTSTATUS WINAPI NtCallbackReturn( void *ret_ptr, ULONG ret_len, NTSTATUS status )
{
    if (!get_syscall_frame()->prev_frame) return STATUS_NO_CALLBACK_ACTIVE;
    user_mode_callback_return( ret_ptr, ret_len, status, NtCurrentTeb() );
}


/***********************************************************************
 * MYIOSDECK (spec 11 Part B): signals deliver Windows exceptions.
 *
 * Everything from here to signal_init_process runs in signal handlers, on the thread's
 * alternate stack: logging goes through ios_sfmt and write(2) (ios_log_write), memory is read
 * with calls that fail instead of faulting, and a fault that must end the pseudo-process sends
 * the thread, once the handler has returned, into ios_end_pseudo_process on its kernel stack.
 */

#define IOS_ALTSTACK_SIZE (128 * 1024)   /* Darwin's SIGSTKSZ */

static struct sigaction prev_actions[NSIG];  /* what the app had installed before (Part B1) */
static size_t altstack_guard;                /* one host page below each alternate stack */
static ULONG_PTR app_image_base;             /* the app's executable, for reading backtraces */

/* one line to ntdll's log, without locks or allocation */
static void sig_log( const char *fmt, ... )
{
    char buffer[512];
    va_list args;
    size_t len;

    len = ios_sfmt( buffer, sizeof(buffer), "ntdll-ios: " );
    va_start( args, fmt );
    len += ios_vsfmt( buffer + len, sizeof(buffer) - len, fmt, args );
    va_end( args );
    ios_log_write( buffer, (int)len );
}

/* the instruction at pc, read with a call that fails instead of faulting */
static int read_insn( ULONG_PTR pc, uint32_t *insn )
{
    vm_size_t size = 0;

    if (pc & 3) return 0;
    if (vm_read_overwrite( mach_task_self(), (vm_address_t)pc, sizeof(*insn), (vm_address_t)insn, &size ))
        return 0;
    return size == sizeof(*insn);
}

/* sets register n (0-30) of the signal context; x18 is the platform's and stays as it is */
static void set_sig_reg( ucontext_t *context, int n, ULONG64 value )
{
    if (n == 18) return;
    if (n < 29) REGn_sig( n, context ) = value;
    else if (n == 29) FP_sig(context) = value;
    else if (n == 30) LR_sig(context) = value;
}


/***********************************************************************
 *           leave_to_app
 *
 * Part B1: a thread with no TEB (the app's UI thread, Metal's threads) isn't a Windows thread.
 * The app's previous action (or the default) goes back in place and the fault happens again
 * there, exactly as without Wine; a signal that was sent rather than caused is sent again.
 */
static BOOL leave_to_app( int signal, siginfo_t *siginfo )
{
    if (ios_current_teb()) return FALSE;
    sigaction( signal, &prev_actions[signal], NULL );
    if (signal == SIGABRT || siginfo->si_code == 0 || siginfo->si_code == SI_USER ||
        siginfo->si_code == SI_QUEUE)
        pthread_kill( pthread_self(), signal );  /* blocked until the handler returns */
    return TRUE;
}


/***********************************************************************
 *           end_process_from_signal
 *
 * Where a thread goes after a handler decided that its pseudo-process ends (Parts B3, B4, B5):
 * on its kernel stack, outside the handler.
 */
static void DECLSPEC_NORETURN end_process_from_signal( NTSTATUS status )
{
    ios_end_pseudo_process( status );
}

static void redirect_to_end( ucontext_t *context, NTSTATUS status )
{
    struct syscall_frame *frame = get_syscall_frame();

    REGn_sig( 0, context ) = (ULONG)status;
    SP_sig(context) = (ULONG_PTR)frame & ~15;  /* the kernel stack below the current frame */
    FP_sig(context) = 0;
    LR_sig(context) = 0;
    PC_sig(context) = (ULONG_PTR)end_process_from_signal;
    context->uc_onstack = 0;                   /* off the alternate stack after the return */
}


/***********************************************************************
 *           log_backtrace
 *
 * Part B3: up to max return addresses of the frame-pointer chain of unix code running on the
 * kernel stack. Only addresses inside the kernel stack are read.
 */
static void log_backtrace( ucontext_t *context, int max )
{
    ULONG_PTR lo = (ULONG_PTR)ntdll_get_thread_data()->kernel_stack, hi = lo + kernel_stack_size;
    ULONG_PTR fp = FP_sig(context);
    int i;

    sig_log( "  app image at %p, pc %p, lr %p, sp %p, fp %p\n", (void *)app_image_base,
             (void *)PC_sig(context), (void *)LR_sig(context), (void *)SP_sig(context), (void *)fp );
    for (i = 0; i < max; i++)
    {
        const ULONG_PTR *f = (const ULONG_PTR *)fp;

        if (fp < lo || fp > hi - 16 || (fp & 7)) break;
        sig_log( "  #%d %p\n", i, (void *)f[1] );
        if (f[0] <= fp) break;
        fp = f[0];
    }
}


/***********************************************************************
 *           fault_in_handler
 *
 * A fault on the alternate stack is a fault of these handlers themselves: no exception can be
 * built from it. The pseudo-process ends.
 */
static BOOL fault_in_handler( ucontext_t *context, int signal, ULONG_PTR addr )
{
    struct arm64_thread_data *data = arm64_thread_data();
    ULONG_PTR sp = SP_sig(context), base = (ULONG_PTR)data->altstack;

    if (!base || sp < base - altstack_guard || sp >= base + data->altstack_size) return FALSE;
    sig_log( "signal %d inside the signal handler of thread %04x, pc %p, address %p: ending the process\n",
             signal, (int)GetCurrentThreadId(), (void *)PC_sig(context), (void *)addr );
    redirect_to_end( context, STATUS_ACCESS_VIOLATION );
    return TRUE;
}


/***********************************************************************
 *           check_storm
 *
 * Part B4: the same fault at the same pc, again and again, means delivery makes no progress.
 * After IOS_STORM_LIMIT identical faults in a row the thread's pseudo-process ends.
 */
static BOOL check_storm( ucontext_t *context, int signal, ULONG_PTR addr, NTSTATUS status )
{
    struct arm64_thread_data *data = arm64_thread_data();
    struct ios_proc *proc = ios_proc_current();

    if (ios_storm_note( &data->storm, signal, PC_sig(context), addr ) < IOS_STORM_LIMIT) return FALSE;
    sig_log( "process %u (%s), thread %04x: %u identical faults in a row (signal %d, pc %p, address %p): ending the process\n",
             proc->serial, proc->name, (int)GetCurrentThreadId(), IOS_STORM_LIMIT, signal,
             (void *)PC_sig(context), (void *)addr );
    ios_storm_reset( &data->storm );
    redirect_to_end( context, status );
    return TRUE;
}


/***********************************************************************
 *           is_unix_fault
 *
 * Part B2/B3: a fault of unix code during a system call: the thread is on its kernel stack and
 * the pc is not in Windows code (a PE image from a signed dylib). The app binary and the system
 * libraries it calls count as unix code.
 */
static BOOL is_unix_fault( ucontext_t *context )
{
    return is_inside_syscall( SP_sig(context) ) && !ios_dylib_find( (void *)PC_sig(context) );
}


/***********************************************************************
 *           setup_raise_exception
 *
 * MYIOSDECK: the context's pc is already where Windows reports it (a breakpoint's own address,
 * Part B2), and x18 isn't touched.
 */
static void setup_raise_exception( ucontext_t *sigcontext, EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct exc_stack_layout *stack;
    void *stack_ptr = (void *)(SP_sig(sigcontext) & ~15);
    NTSTATUS status;

    status = send_debug_event( rec, context, TRUE, TRUE );
    if (status == DBG_CONTINUE || status == DBG_EXCEPTION_HANDLED)
    {
        restore_context( context, sigcontext );
        return;
    }

    stack = virtual_setup_exception( stack_ptr, sizeof(*stack), rec );
    stack->rec = *rec;
    stack->context = *context;
    context_init_empty_xstate( &stack->context, stack->redzone );

    SP_sig(sigcontext) = (ULONG_PTR)stack;
    PC_sig(sigcontext) = (ULONG_PTR)pKiUserExceptionDispatcher;
}


/***********************************************************************
 *           setup_exception
 *
 * Modify the signal context to call the exception raise function.
 */
static void setup_exception( ucontext_t *sigcontext, EXCEPTION_RECORD *rec )
{
    CONTEXT context;

    rec->ExceptionAddress = (void *)PC_sig(sigcontext);
    save_context( &context, sigcontext );
    setup_raise_exception( sigcontext, rec, &context );
}


/* unix-side faults outside __TRY so far in this app run (clarification 3's log budget) */
static LONG unix_fault_count;

/***********************************************************************
 *           handle_syscall_fault
 *
 * Handle a fault happening during a system call.
 * MYIOSDECK (spec 11 Part B3 and clarification 3): inside a __TRY block (jmp_buf set) the call
 * unwinds to its __EXCEPT. Any other fault of unix code ends the system call with the exception
 * code as its status and returns to the caller's frame, as upstream does: Windows answers a bad
 * caller pointer with a status. The first 16 such faults of the app run, then every 256th, are
 * logged with the pc, the address and a short backtrace. With no syscall frame to return to the
 * fault stays a Windows exception (Part B2).
 */
static BOOL handle_syscall_fault( ucontext_t *context, EXCEPTION_RECORD *rec )
{
    struct syscall_frame *frame = get_syscall_frame();
    struct ios_proc *proc;
    LONG count;

    if (!frame || !is_unix_fault( context )) return FALSE;

    TRACE( "code=%x flags=%x addr=%p pc=%p tid=%04x\n",
           rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress,
           (void *)PC_sig(context), GetCurrentThreadId() );

    if (ntdll_get_thread_data()->jmp_buf)
    {
        TRACE( "returning to handler\n" );
        REGn_sig(0, context) = (ULONG_PTR)ntdll_get_thread_data()->jmp_buf;
        REGn_sig(1, context) = 1;
        PC_sig(context)      = (ULONG_PTR)longjmp;
        ntdll_get_thread_data()->jmp_buf = NULL;
        return TRUE;
    }

    count = InterlockedIncrement( &unix_fault_count );
    if (count <= 16 || !(count % 256))
    {
        proc = ios_proc_current();
        sig_log( "process %u (%s), thread %04x: exception %x in a system call at pc %p, address %p: "
                 "the call returns it as its status (fault %d of this run)\n",
                 proc->serial, proc->name, (int)GetCurrentThreadId(), (unsigned int)rec->ExceptionCode,
                 (void *)PC_sig(context), (void *)rec->ExceptionInformation[1], (int)count );
        log_backtrace( context, 8 );
    }

    TRACE( "returning to user mode ip=%p ret=%08x\n", (void *)frame->pc, rec->ExceptionCode );
    REGn_sig(0, context) = rec->ExceptionCode;
    SP_sig(context)      = (ULONG_PTR)frame;
    PC_sig(context)      = (ULONG_PTR)__wine_syscall_dispatcher_return;
    return TRUE;
}


/***********************************************************************
 *           record_from
 */
static void record_from( EXCEPTION_RECORD *rec, const struct ios_exc *e )
{
    rec->ExceptionCode = e->code;
    rec->ExceptionFlags = e->flags;
    rec->NumberParameters = e->nparams;
    rec->ExceptionInformation[0] = e->info[0];
    rec->ExceptionInformation[1] = e->info[1];
}


/**********************************************************************
 *		segv_handler
 *
 * Handler for SIGSEGV and SIGBUS (Part B2: both are access violations).
 */
static void segv_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    ULONG_PTR addr = (ULONG_PTR)siginfo->si_addr;
    struct ios_exc e;

    if (leave_to_app( signal, siginfo )) return;
    if (fault_in_handler( context, signal, addr )) return;

    /* clarification 1: a data fault in a low window is serviced and the thread resumes */
    if (addr < ((ULONG_PTR)1 << 32) &&
        ios_lowwin_service( SS_sig(context), NS_sig(context), addr, NtCurrentTeb()->Peb ) == IOS_LW_DONE)
    {
        ios_storm_reset( &arm64_thread_data()->storm );
        return;
    }

    if (check_storm( context, signal, addr, STATUS_ACCESS_VIOLATION )) return;

    ios_sig_exception( signal == SIGBUS ? IOS_SIGK_BUS : IOS_SIGK_SEGV, IOS_SUB_NONE,
                       get_fault_esr( context ), addr, PC_sig(context), 0, 0, 0, &e );
    record_from( &rec, &e );
    if (!virtual_handle_fault( &rec, (void *)SP_sig(context) )) return;
    if (handle_syscall_fault( context, &rec )) return;
    setup_exception( context, &rec );
}


/**********************************************************************
 *		ill_handler
 *
 * Handler for SIGILL.
 */
static void ill_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    struct ios_exc e;
    uint32_t insn = 0;
    int insn_ok;

    if (leave_to_app( signal, siginfo )) return;
    if (fault_in_handler( context, signal, PC_sig(context) )) return;

    insn_ok = read_insn( PC_sig(context), &insn );
    ios_sig_exception( IOS_SIGK_ILL, IOS_SUB_NONE, get_fault_esr( context ), PC_sig(context),
                       PC_sig(context), insn, insn_ok, REGn_sig(0, context), &e );
    if (e.action == IOS_EXC_EMULATED)  /* mrs xN, CurrentEL */
    {
        if (e.emul_reg >= 0 && e.emul_reg != 31) set_sig_reg( context, e.emul_reg, 0 );
        PC_sig(context) += e.pc_adjust;
        ios_storm_reset( &arm64_thread_data()->storm );
        return;
    }
    if (check_storm( context, signal, PC_sig(context), e.code )) return;
    record_from( &rec, &e );
    if (handle_syscall_fault( context, &rec )) return;
    setup_exception( context, &rec );
}


/***********************************************************************
 *           restore_frame_to_sigcontext
 *
 * MYIOSDECK (spec 11 Part A2): the dispatcher's full-context return (upstream's usr2_handler):
 * every register from the syscall frame at once, x18 excepted. The floating-point registers
 * only when they are being restored.
 */
static void restore_frame_to_sigcontext( ucontext_t *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    DWORD i;

    FP_sig(context)     = frame->fp;
    LR_sig(context)     = frame->lr;
    SP_sig(context)     = frame->sp;
    PC_sig(context)     = frame->pc;
    PSTATE_sig(context) = frame->cpsr;
    for (i = 0; i <= 28; i++) if (i != 18) REGn_sig( i, context ) = frame->x[i];
    if (frame->restore_flags & CONTEXT_FLOATING_POINT & ~CONTEXT_ARM64)
    {
        context->uc_mcontext->__ns.__fpcr = frame->fpcr;
        context->uc_mcontext->__ns.__fpsr = frame->fpsr;
        memcpy( context->uc_mcontext->__ns.__v, frame->v, sizeof(frame->v) );
    }
}


/**********************************************************************
 *		trap_handler
 *
 * Handler for SIGTRAP.
 * MYIOSDECK (spec 11 Part B2): the brk immediate comes from the syndrome (EC 0x3c) or, failing
 * that, from the instruction; brk #0xf000 and unknown immediates are breakpoints, #0xf003 is
 * __fastfail. The exception reports the brk's own address in the context, as Windows does.
 */
static void trap_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    struct arm64_thread_data *data;
    DWORD64 esr = get_fault_esr( context );
    struct ios_exc e;
    uint32_t insn = 0;
    int insn_ok;
    CONTEXT ctx;

    if (leave_to_app( signal, siginfo )) return;

    data = arm64_thread_data();
    if (data->full_restore && is_inside_syscall( SP_sig(context) ))
    {
        data->full_restore = 0;
        restore_frame_to_sigcontext( context );
        return;
    }
    if (fault_in_handler( context, signal, PC_sig(context) )) return;

    if (((esr >> 26) & 0x3f) == 0x3c)  /* BRK in AArch64: ISS[15:0] is the immediate */
    {
        insn = 0xd4200000 | (((uint32_t)esr & 0xffff) << 5);
        insn_ok = 1;
    }
    else insn_ok = read_insn( PC_sig(context), &insn );

    ios_sig_exception( IOS_SIGK_TRAP, siginfo->si_code == TRAP_TRACE ? IOS_TRAP_TRACE : IOS_TRAP_BRK,
                       esr, PC_sig(context), PC_sig(context), insn, insn_ok, REGn_sig(0, context), &e );
    if (check_storm( context, signal, PC_sig(context), e.code )) return;

    if (e.action == IOS_EXC_FASTFAIL)
    {
        struct ios_proc *proc = ios_proc_current();

        /* Windows ends the process at once, with STATUS_STACK_BUFFER_OVERRUN */
        sig_log( "process %u (%s), thread %04x: __fastfail(%u) at pc %p: ending the process\n",
                 proc->serial, proc->name, (int)GetCurrentThreadId(), (unsigned int)e.info[0],
                 (void *)PC_sig(context) );
        redirect_to_end( context, e.code );
        return;
    }

    record_from( &rec, &e );
    rec.ExceptionAddress = (void *)PC_sig(context);
    if (handle_syscall_fault( context, &rec )) return;
    save_context( &ctx, sigcontext );
    setup_raise_exception( sigcontext, &rec, &ctx );
}


/**********************************************************************
 *		fpe_handler
 *
 * Handler for SIGFPE.
 */
static void fpe_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    struct ios_exc e;
    int sub;

    if (leave_to_app( signal, siginfo )) return;
    if (fault_in_handler( context, signal, PC_sig(context) )) return;

    switch (siginfo->si_code & 0xffff)
    {
    case FPE_FLTSUB: sub = IOS_FPE_FLTSUB; break;
    case FPE_INTDIV: sub = IOS_FPE_INTDIV; break;
    case FPE_INTOVF: sub = IOS_FPE_INTOVF; break;
    case FPE_FLTDIV: sub = IOS_FPE_FLTDIV; break;
    case FPE_FLTOVF: sub = IOS_FPE_FLTOVF; break;
    case FPE_FLTUND: sub = IOS_FPE_FLTUND; break;
    case FPE_FLTRES: sub = IOS_FPE_FLTRES; break;
    default:         sub = IOS_FPE_FLTINV; break;
    }
    ios_sig_exception( IOS_SIGK_FPE, sub, get_fault_esr( context ), PC_sig(context), PC_sig(context),
                       0, 0, 0, &e );
    if (check_storm( context, signal, PC_sig(context), e.code )) return;
    record_from( &rec, &e );
    if (handle_syscall_fault( context, &rec )) return;
    setup_exception( context, &rec );
}


/**********************************************************************
 *		abrt_handler
 *
 * Handler for SIGABRT.
 * MYIOSDECK (spec 11 Part B5): abort() of a system library on a Windows thread ends that
 * pseudo-process (spec 09's "no exits"), with abort's exit code.
 */
static void abrt_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    ucontext_t *context = sigcontext;
    struct ios_proc *proc;

    if (leave_to_app( signal, siginfo )) return;
    proc = ios_proc_current();
    sig_log( "process %u (%s), thread %04x: SIGABRT at pc %p: ending the process\n",
             proc->serial, proc->name, (int)GetCurrentThreadId(), (void *)PC_sig(context) );
    if (is_inside_syscall( SP_sig(context) )) log_backtrace( context, 32 );
    redirect_to_end( context, IOS_ABORT_EXIT_CODE );
}


/**********************************************************************
 *           get_thread_ldt_entry
 */
NTSTATUS get_thread_ldt_entry( HANDLE handle, THREAD_DESCRIPTOR_INFORMATION *info, ULONG len )
{
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *             signal_init_threading
 */
void signal_init_threading(void)
{
}


/**********************************************************************
 *             signal_alloc_thread
 *
 * MYIOSDECK (spec 11 Part B): the thread's alternate signal stack, with a guard page below it.
 * The thread itself turns it on (enable_altstack).
 */
NTSTATUS signal_alloc_thread( TEB *teb )
{
    struct arm64_thread_data *data = arm64_thread_data_of( teb );
    size_t guard = altstack_guard ? altstack_guard : (size_t)getpagesize();
    char *ptr;

    if (data->altstack) return STATUS_SUCCESS;
    if ((ptr = anon_mmap_alloc( guard + IOS_ALTSTACK_SIZE, PROT_READ | PROT_WRITE )) == MAP_FAILED)
        return STATUS_NO_MEMORY;
    mprotect( ptr, guard, PROT_NONE );
    data->altstack = ptr + guard;
    data->altstack_size = IOS_ALTSTACK_SIZE;
    return STATUS_SUCCESS;
}


/**********************************************************************
 *             signal_free_thread
 *
 * The thread is gone (upstream frees a TEB after its thread has left).
 */
void signal_free_thread( TEB *teb )
{
    struct arm64_thread_data *data = arm64_thread_data_of( teb );
    size_t guard = altstack_guard ? altstack_guard : (size_t)getpagesize();

    if (data->altstack) munmap( (char *)data->altstack - guard, guard + data->altstack_size );
    memset( data, 0, sizeof(*data) );
}


/* the calling thread runs its signal handlers on its alternate stack from now on */
static void enable_altstack( TEB *teb )
{
    struct arm64_thread_data *data = arm64_thread_data_of( teb );
    stack_t ss;

    if (!data->altstack && signal_alloc_thread( teb )) return;
    ss.ss_sp    = data->altstack;
    ss.ss_size  = data->altstack_size;
    ss.ss_flags = 0;
    if (sigaltstack( &ss, NULL ) == -1)
        sig_log( "thread %04x: no alternate signal stack\n", (int)GetCurrentThreadId() );
}


/***********************************************************************
 * MYIOSDECK (spec 11 Part D): the thread registry, {Mach thread port, TEB} of every Windows
 * thread (ios_sig_pure.c: 1024 slots, lock-free readers, reusable slots, exact lookups). The
 * app reads it through these four functions; ios_mach_exc.c and spec 10 (virtual.c) use it too.
 */
int ios_thread_registry_count(void)
{
    return ios_threadreg_count();
}

uintptr_t ios_thread_registry_teb( int i )
{
    return ios_threadreg_teb( i );
}

thread_t ios_thread_registry_mach( int i )
{
    return ios_threadreg_mach( i );
}

int ios_thread_registry_range_busy( uintptr_t base, uintptr_t size )
{
    return ios_threadreg_range_busy( base, size );
}

/* Part D1: the calling thread registers itself; the port name is the one the server knows it
 * by (server.c's get_unix_tid), the id makes exception messages match whatever name they carry */
static void thread_register( TEB *teb )
{
    struct arm64_thread_data *data = arm64_thread_data_of( teb );
    mach_port_t port = mach_thread_self();
    uint64_t id = 0;

    mach_port_deallocate( mach_task_self(), port );  /* the thread keeps its own right to it */
    pthread_threadid_np( NULL, &id );
    if (data->mach_port && data->mach_port != port) ios_threadreg_remove( data->mach_port );
    if (ios_threadreg_add( port, id, (uintptr_t)teb ))
    {
        sig_log( "thread registry full: thread %04x's faults go to signals only\n", (int)GetCurrentThreadId() );
        data->mach_port = 0;
        return;
    }
    data->mach_port = port;
}

/* Part D1: ... and removes itself when it leaves; the slot can be reused */
static void thread_unregister( TEB *teb )
{
    struct arm64_thread_data *data = arm64_thread_data_of( teb );

    if (!data->mach_port) return;
    ios_threadreg_remove( data->mach_port );
    data->mach_port = 0;
}


/***********************************************************************
 *           ios_signal_thread_start
 *
 * Part A3: a new thread's set-up, on the thread, before it runs Windows code: its alternate
 * signal stack (Part B) and its registry entry (Part D).
 */
static void ios_signal_thread_start( TEB *teb )
{
    enable_altstack( teb );
    ios_storm_reset( &arm64_thread_data_of( teb )->storm );
    thread_register( teb );
}


/***********************************************************************
 *           ios_signal_thread_leave
 *
 * Spec 09 Part E5 (ios_thread_leave): a Windows thread leaves. It leaves the registry (Part D)
 * and its alternate stack is turned off (the memory goes with the TEB, and the session's thread
 * returns to the app).
 */
void ios_signal_thread_leave(void)
{
    TEB *teb = ios_current_teb();
    stack_t ss;

    if (!teb) return;
    thread_unregister( teb );
    memset( &ss, 0, sizeof(ss) );
    ss.ss_flags = SS_DISABLE;
    sigaltstack( &ss, NULL );
}


/* installs one handler and keeps the app's previous action (Part B1) */
static void install_handler( int signal, void (*handler)( int, siginfo_t *, void * ) )
{
    struct sigaction sig_act;

    memset( &sig_act, 0, sizeof(sig_act) );
    sig_act.sa_mask = server_block_set;
    sig_act.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sig_act.sa_sigaction = handler;
    if (sigaction( signal, &sig_act, &prev_actions[signal] ) == -1)
        sig_log( "sigaction(%d) failed: Windows exceptions from this signal won't work\n", signal );
}


/**********************************************************************
 *		signal_init_process
 *
 * MYIOSDECK (spec 09 Part B, spec 11 Part B): sets up the calling thread for every
 * pseudo-process, but installs the handlers once for the app.
 */
void signal_init_process(void)
{
    struct ntdll_thread_data *thread_data = ntdll_get_thread_data();
    void *kernel_stack = (char *)thread_data->kernel_stack + kernel_stack_size;
    static LONG installed;

    thread_data->syscall_frame = (struct syscall_frame *)kernel_stack - 1;

    if (!InterlockedExchange( &installed, 1 ))
    {
        altstack_guard = getpagesize();
        app_image_base = (ULONG_PTR)_dyld_get_image_header( 0 );

        install_handler( SIGSEGV, segv_handler );
        install_handler( SIGBUS, segv_handler );
        install_handler( SIGILL, ill_handler );
        install_handler( SIGTRAP, trap_handler );
        install_handler( SIGFPE, fpe_handler );
        install_handler( SIGABRT, abrt_handler );
        /* Part B5: no SIGUSR1/SIGUSR2 handlers (spec 08 Part E captures contexts through Mach);
         * SIGINT, SIGQUIT and SIGTERM are left as the app has them (spec 08 ignores them) */

        /* Part C1: the interpreter's faults first, before the first Windows thread runs */
        ios_mach_exc_init();
    }

    signal_alloc_thread( NtCurrentTeb() );
    enable_altstack( NtCurrentTeb() );
}


/***********************************************************************
 *           syscall_dispatcher_return_slowpath
 *
 * MYIOSDECK (spec 11 Parts A2, B5): the return needs every register from the frame at once,
 * x16 and x17 included. Upstream raises SIGUSR2, which has no handler here; the thread sends
 * itself a SIGTRAP instead and the SIGTRAP handler loads the frame into the signal context
 * (restore_frame_to_sigcontext). Called on the kernel stack, sp = the frame.
 */
void syscall_dispatcher_return_slowpath(void)
{
    arm64_thread_data()->full_restore = 1;
    pthread_kill( pthread_self(), SIGTRAP );
    /* not reached unless SIGTRAP was blocked: the fast path then restores what it can */
    arm64_thread_data()->full_restore = 0;
}


/***********************************************************************
 *           ios_cpu_area_sync
 *
 * MYIOSDECK (spec 09 Part E6, moved here from signal_arm64.c): the server reads a thread's guest
 * registers from memory the thread keeps up to date itself (spec 08 Part E1). For an ARM64EC
 * thread that is the x64 context of its CPU area, refreshed here from the syscall frame at every
 * system-call entry (args: x0-x8 as they came in) and return (args NULL: the frame as it will be
 * restored, NtSetContextThread included). The registers follow the ARM64EC mapping (Rax=x8,
 * Rcx=x0, Rdx=x1, Rbx=x27, Rsp=sp, Rbp=fp, Rsi=x25, Rdi=x26, R8-R11=x2-x5, R12-R15=x19-x22,
 * Xmm0-15=v0-v15). A WoW64 thread's i386 context lives in its CPU-reserved block, which the
 * CPU module writes when it leaves guest code for the system call; nothing to do for it here.
 * Called from the dispatcher, on the kernel stack.
 */
void ios_cpu_area_sync( struct syscall_frame *frame, const ULONG64 *args )
{
    TEB *teb = ios_current_teb();
    CHPE_V2_CPU_AREA_INFO *cpu;
    ARM64EC_NT_CONTEXT *ctx;

    if (!teb || !(cpu = teb->ChpeV2CpuAreaInfo) || !(ctx = cpu->ContextAmd64)) return;
    if (cpu->InSimulation) return;  /* the emulator's own state is the current one */

    if (args)
    {
        ctx->X0 = args[0];
        ctx->X1 = args[1];
        ctx->X2 = args[2];
        ctx->X3 = args[3];
        ctx->X4 = args[4];
        ctx->X5 = args[5];
        ctx->X8 = args[8];
    }
    else if (frame->restore_flags & (CONTEXT_INTEGER & ~CONTEXT_ARM64))  /* the bit the dispatcher tests */
    {
        ctx->X0 = frame->x[0];
        ctx->X1 = frame->x[1];
        ctx->X2 = frame->x[2];
        ctx->X3 = frame->x[3];
        ctx->X4 = frame->x[4];
        ctx->X5 = frame->x[5];
        ctx->X8 = frame->x[8];
    }
    ctx->X19 = frame->x[19];
    ctx->X20 = frame->x[20];
    ctx->X21 = frame->x[21];
    ctx->X22 = frame->x[22];
    ctx->X25 = frame->x[25];
    ctx->X26 = frame->x[26];
    ctx->X27 = frame->x[27];
    ctx->Fp  = frame->fp;
    ctx->Sp  = frame->sp;
    ctx->Pc  = frame->pc;
    ctx->Lr  = frame->lr;
    memcpy( ctx->V, frame->v, sizeof(ctx->V) );
}


/***********************************************************************
 *           init_syscall_frame
 *
 * MYIOSDECK (spec 11 Part A3): runs on the new thread itself, before it first runs Windows
 * code. The thread gets its alternate signal stack and registers itself (Part D); x18 is left
 * alone (frame->x[18] only reports the TEB).
 */
void init_syscall_frame( LPTHREAD_START_ROUTINE entry, void *arg, BOOL suspend, TEB *teb )
{
    struct syscall_frame *frame = ((struct ntdll_thread_data *)&teb->GdiTebBatch)->syscall_frame;
    CONTEXT *ctx, context = { CONTEXT_ALL };
    I386_CONTEXT *i386_context;
    ARM_CONTEXT *arm_context;

    ios_signal_thread_start( teb );

    context.X0  = (DWORD64)entry;
    context.X1  = (DWORD64)arg;
    context.X18 = (DWORD64)teb;
    context.Sp  = (DWORD64)teb->Tib.StackBase;
    context.Pc  = (DWORD64)pRtlUserThreadStart;

    if ((i386_context = get_cpu_area( IMAGE_FILE_MACHINE_I386 )))
    {
        XMM_SAVE_AREA32 *fpu = (XMM_SAVE_AREA32 *)i386_context->ExtendedRegisters;
        i386_context->ContextFlags = CONTEXT_I386_ALL;
        i386_context->Eax = (ULONG_PTR)entry;
        i386_context->Ebx = (arg == ios_cur_peb() ? (ULONG_PTR)wow_peb : (ULONG_PTR)arg);
        i386_context->Esp = get_wow_teb( teb )->Tib.StackBase - 16;
        i386_context->Eip = ios_wow64_thread_start();  /* the block's pRtlUserThreadStart (spec 09) */
        i386_context->SegCs = 0x23;
        i386_context->SegDs = 0x2b;
        i386_context->SegEs = 0x2b;
        i386_context->SegFs = 0x53;
        i386_context->SegGs = 0x2b;
        i386_context->SegSs = 0x2b;
        i386_context->EFlags = 0x202;
        fpu->ControlWord = 0x27f;
        fpu->MxCsr = 0x1f80;
        fpux_to_fpu( &i386_context->FloatSave, fpu );
    }
    else if ((arm_context = get_cpu_area( IMAGE_FILE_MACHINE_ARMNT )))
    {
        arm_context->ContextFlags = CONTEXT_ARM_ALL;
        arm_context->R0 = (ULONG_PTR)entry;
        arm_context->R1 = (arg == ios_cur_peb() ? (ULONG_PTR)wow_peb : (ULONG_PTR)arg);
        arm_context->Sp = get_wow_teb( teb )->Tib.StackBase;
        arm_context->Pc = ios_wow64_thread_start();  /* the block's pRtlUserThreadStart (spec 09) */
        if (arm_context->Pc & 1) arm_context->Cpsr |= 0x20; /* thumb mode */
    }

    if (suspend)
    {
        context.ContextFlags |= CONTEXT_EXCEPTION_REPORTING | CONTEXT_EXCEPTION_ACTIVE;
        wait_suspend( &context );
    }

    ctx = (CONTEXT *)((ULONG_PTR)context.Sp & ~15) - 1;
    *ctx = context;
    ctx->ContextFlags = CONTEXT_FULL;
    signal_set_full_context( ctx );

    frame->sp    = (ULONG64)ctx;
    frame->pc    = (ULONG64)pLdrInitializeThunk;
    frame->x[0]  = (ULONG64)ctx;
    frame->x[18] = (ULONG64)teb;
    syscall_frame_fixup_for_fastpath( frame );

    pthread_sigmask( SIG_UNBLOCK, &server_block_set, NULL );
}


/***********************************************************************
 *           signal_start_thread
 */
__ASM_GLOBAL_FUNC( signal_start_thread,
                   "stp x29, x30, [sp,#-0xc0]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0xc0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xb8\n\t")
                   "mov x29, sp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   "stp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   "stp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   "stp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   "stp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   "stp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "add x5, x29, #0xc0\n\t"     /* syscall_cfa */
                   /* set syscall frame */
                   "ldr x4, [x3, #0x378]\n\t"   /* thread_data->syscall_frame */
                   "cbnz x4, 1f\n\t"
                   "sub x4, sp, #0x330\n\t"     /* sizeof(struct syscall_frame) */
                   "str x4, [x3, #0x378]\n\t"   /* thread_data->syscall_frame */
                   "1:\tstr wzr, [x4, #0x10c]\n\t" /* frame->restore_flags */
                   "stp xzr, x5, [x4, #0x110]\n\t" /* frame->prev_frame,syscall_cfa */
                   /* switch to kernel stack */
                   "mov sp, x4\n\t"
                   "bl " __ASM_NAME("init_syscall_frame") "\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )


/* MYIOSDECK (spec 09 Part E6): refresh the CPU area at system-call entry, on the kernel stack
 * with x22 = the frame; x0-x8 (the arguments and the syscall number) are kept */
#define IOS_SYSCALL_ENTRY_SYNC \
                   "sub sp, sp, #0x50\n\t" \
                   "stp x0, x1, [sp]\n\t" \
                   "stp x2, x3, [sp, #0x10]\n\t" \
                   "stp x4, x5, [sp, #0x20]\n\t" \
                   "stp x6, x7, [sp, #0x30]\n\t" \
                   "str x8, [sp, #0x40]\n\t" \
                   "mov x0, x22\n\t"            /* frame */ \
                   "mov x1, sp\n\t"             /* args */ \
                   "bl " __ASM_NAME("ios_cpu_area_sync") "\n\t" \
                   "ldp x0, x1, [sp]\n\t" \
                   "ldp x2, x3, [sp, #0x10]\n\t" \
                   "ldp x4, x5, [sp, #0x20]\n\t" \
                   "ldp x6, x7, [sp, #0x30]\n\t" \
                   "ldr x8, [sp, #0x40]\n\t" \
                   "add sp, sp, #0x50\n\t"
/* ... and on the way back, from what the frame restores; x0 (the return value) is kept.
 * sp moves for the call, so an unwind through ios_cpu_area_sync would see a wrong CFA; nothing
 * in it can fault. */
#define IOS_SYSCALL_RETURN_SYNC \
                   "str x0, [sp, #-0x10]!\n\t" \
                   "add x0, sp, #0x10\n\t"      /* frame */ \
                   "mov x1, #0\n\t" \
                   "bl " __ASM_NAME("ios_cpu_area_sync") "\n\t" \
                   "ldr x0, [sp], #0x10\n\t"


/***********************************************************************
 *           __wine_syscall_dispatcher
 *
 * MYIOSDECK (spec 11 Part A2): upstream's, with the TEB from TSD (kept in x24, which the frame
 * saves and restores, in place of x18) and x18 neither saved nor restored.
 */
__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher,
                   "hint 34\n\t" /* bti c */
                   IOS_ASM_LOAD_TEB("x11", "x10")
                   "ldr x10, [x11, #0x378]\n\t" /* thread_data->syscall_frame */
                   "str x19, [x10, #0x98]\n\t"  /* x18 (0x90) is the platform's */
                   "stp x20, x21, [x10, #0xa0]\n\t"
                   "stp x22, x23, [x10, #0xb0]\n\t"
                   "stp x24, x25, [x10, #0xc0]\n\t"
                   "stp x26, x27, [x10, #0xd0]\n\t"
                   "stp x28, x29, [x10, #0xe0]\n\t"
                   "mov x19, sp\n\t"
                   "stp x9, x19, [x10, #0xf0]\n\t"
                   "mrs x9, NZCV\n\t"
                   "stp x30, x9, [x10, #0x100]\n\t"
                   "str w8, [x10, #0x120]\n\t"
                   "mrs x9, FPCR\n\t"
                   "str w9, [x10, #0x128]\n\t"
                   "mrs x9, FPSR\n\t"
                   "str w9, [x10, #0x12c]\n\t"
                   "stp q0,  q1,  [x10, #0x130]\n\t"
                   "stp q2,  q3,  [x10, #0x150]\n\t"
                   "stp q4,  q5,  [x10, #0x170]\n\t"
                   "stp q6,  q7,  [x10, #0x190]\n\t"
                   "stp q8,  q9,  [x10, #0x1b0]\n\t"
                   "stp q10, q11, [x10, #0x1d0]\n\t"
                   "stp q12, q13, [x10, #0x1f0]\n\t"
                   "stp q14, q15, [x10, #0x210]\n\t"
                   "stp q16, q17, [x10, #0x230]\n\t"
                   "stp q18, q19, [x10, #0x250]\n\t"
                   "stp q20, q21, [x10, #0x270]\n\t"
                   "stp q22, q23, [x10, #0x290]\n\t"
                   "stp q24, q25, [x10, #0x2b0]\n\t"
                   "stp q26, q27, [x10, #0x2d0]\n\t"
                   "stp q28, q29, [x10, #0x2f0]\n\t"
                   "stp q30, q31, [x10, #0x310]\n\t"
                   "mov x22, x10\n\t"
                   "mov x24, x11\n\t"           /* teb */
                   /* switch to kernel stack */
                   "mov sp, x10\n\t"
                   /* we're now on the kernel stack, stitch unwind info with previous frame */
                   __ASM_CFI_CFA_IS_AT2(x22, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_CFI(".cfi_offset 29, -0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30, -0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19, -0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20, -0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21, -0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22, -0x98\n\t")
                   __ASM_CFI(".cfi_offset 23, -0x90\n\t")
                   __ASM_CFI(".cfi_offset 24, -0x88\n\t")
                   __ASM_CFI(".cfi_offset 25, -0x80\n\t")
                   __ASM_CFI(".cfi_offset 26, -0x78\n\t")
                   __ASM_CFI(".cfi_offset 27, -0x70\n\t")
                   __ASM_CFI(".cfi_offset 28, -0x68\n\t")
                   IOS_SYSCALL_ENTRY_SYNC
                   "and x20, x8, #0xfff\n\t"    /* syscall number */
                   "ubfx x21, x8, #12, #2\n\t"  /* syscall table number */
                   "ldr x16, [x24, #0x370]\n\t" /* thread_data->syscall_table */
                   "add x21, x16, x21, lsl #5\n\t"
                   "ldr x16, [x21, #16]\n\t"    /* table->ServiceLimit */
                   "cmp x20, x16\n\t"
                   "bcs " __ASM_LOCAL_LABEL("bad_syscall") "\n\t"
                   "ldr x16, [x21, #24]\n\t"    /* table->ArgumentTable */
                   "ldrb w9, [x16, x20]\n\t"
                   "subs x9, x9, #64\n\t"
                   "bls 2f\n\t"
                   "sub sp, sp, x9\n\t"
                   "tbz x9, #3, 1f\n\t"
                   "sub sp, sp, #8\n"
                   "1:\tsub x9, x9, #8\n\t"
                   "ldr x10, [x19, x9]\n\t"
                   "str x10, [sp, x9]\n\t"
                   "cbnz x9, 1b\n"
                   "2:\tldr x16, [x21]\n\t"     /* table->ServiceTable */
                   "ldr x23, [x16, x20, lsl 3]\n\t"
                   "ldr w11, [x24, #0x380]\n\t" /* thread_data->syscall_trace */
                   "cbnz x11, " __ASM_LOCAL_LABEL("trace_syscall") "\n\t"
                   "blr x23\n\t"
                   "mov sp, x22\n"
                   __ASM_CFI_CFA_IS_AT2(sp, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") ":\n\t"
                   IOS_SYSCALL_RETURN_SYNC
                   "ldr w16, [sp, #0x10c]\n\t"  /* frame->restore_flags */
                   "tbz x16, #1, 2f\n\t"        /* CONTEXT_INTEGER */
                   "ldp x12, x13, [sp, #0x80]\n\t" /* frame->x[16..17] */
                   "ldp x14, x15, [sp, #0xf8]\n\t" /* frame->sp, frame->pc */
                   "cmp x12, x15\n\t"              /* frame->x16 == frame->pc? */
                   "ccmp x13, x14, #0, eq\n\t"     /* frame->x17 == frame->sp? */
                   "beq 1f\n\t"                    /* take slowpath if unequal */
                   "bl " __ASM_NAME("syscall_dispatcher_return_slowpath") "\n"
                   "1:\tldp x0, x1, [sp, #0x00]\n\t"
                   "ldp x2, x3, [sp, #0x10]\n\t"
                   "ldp x4, x5, [sp, #0x20]\n\t"
                   "ldp x6, x7, [sp, #0x30]\n\t"
                   "ldp x8, x9, [sp, #0x40]\n\t"
                   "ldp x10, x11, [sp, #0x50]\n\t"
                   "ldp x12, x13, [sp, #0x60]\n\t"
                   "ldp x14, x15, [sp, #0x70]\n"
                   "2:\tldr x19, [sp, #0x98]\n\t" /* x18 is not restored */
                   "ldp x20, x21, [sp, #0xa0]\n\t"
                   "ldp x22, x23, [sp, #0xb0]\n\t"
                   "ldp x24, x25, [sp, #0xc0]\n\t"
                   "ldp x26, x27, [sp, #0xd0]\n\t"
                   "ldp x28, x29, [sp, #0xe0]\n\t"
                   "tbz x16, #2, 1f\n\t"        /* CONTEXT_FLOATING_POINT */
                   "ldp q0,  q1,  [sp, #0x130]\n\t"
                   "ldp q2,  q3,  [sp, #0x150]\n\t"
                   "ldp q4,  q5,  [sp, #0x170]\n\t"
                   "ldp q6,  q7,  [sp, #0x190]\n\t"
                   "ldp q8,  q9,  [sp, #0x1b0]\n\t"
                   "ldp q10, q11, [sp, #0x1d0]\n\t"
                   "ldp q12, q13, [sp, #0x1f0]\n\t"
                   "ldp q14, q15, [sp, #0x210]\n\t"
                   "ldp q16, q17, [sp, #0x230]\n\t"
                   "ldp q18, q19, [sp, #0x250]\n\t"
                   "ldp q20, q21, [sp, #0x270]\n\t"
                   "ldp q22, q23, [sp, #0x290]\n\t"
                   "ldp q24, q25, [sp, #0x2b0]\n\t"
                   "ldp q26, q27, [sp, #0x2d0]\n\t"
                   "ldp q28, q29, [sp, #0x2f0]\n\t"
                   "ldp q30, q31, [sp, #0x310]\n\t"
                   "ldr w17, [sp, #0x128]\n\t"
                   "msr FPCR, x17\n\t"
                   "ldr w17, [sp, #0x12c]\n\t"
                   "msr FPSR, x17\n"
                   "1:\tldp x16, x17, [sp, #0x100]\n\t"
                   "msr NZCV, x17\n\t"
                   "ldp x30, x17, [sp, #0xf0]\n\t"
                   /* switch to user stack */
                   "mov sp, x17\n\t"
                   "ret x16\n"

                   __ASM_LOCAL_LABEL("trace_syscall") ":\n\t"
                   "stp x0, x1, [sp, #-0x40]!\n\t"
                   "stp x2, x3, [sp, #0x10]\n\t"
                   "stp x4, x5, [sp, #0x20]\n\t"
                   "stp x6, x7, [sp, #0x30]\n\t"
                   "mov x0, x8\n\t"             /* id */
                   "mov x1, sp\n\t"             /* args */
                   "ldr x16, [x21, #24]\n\t"    /* table->ArgumentTable */
                   "ldrb w2, [x16, x20]\n\t"    /* len */
                   "bl " __ASM_NAME("trace_syscall") "\n\t"
                   "ldp x2, x3, [sp, #0x10]\n\t"
                   "ldp x4, x5, [sp, #0x20]\n\t"
                   "ldp x6, x7, [sp, #0x30]\n\t"
                   "ldp x0, x1, [sp], #0x40\n\t"
                   "blr x23\n"
                   "mov sp, x22\n"

                   __ASM_LOCAL_LABEL("trace_syscall_ret") ":\n\t"
                   "mov x21, x0\n\t"            /* retval */
                   "ldr w0, [sp, #0x120]\n\t"   /* frame->syscall_id */
                   "mov x1, x21\n\t"            /* retval */
                   "bl " __ASM_NAME("trace_sysret") "\n\t"
                   "mov x0, x21\n\t"            /* retval */
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") "\n"

                   __ASM_LOCAL_LABEL("bad_syscall") ":\n\t"
                   "mov x0, #0xc0000000\n\t"    /* STATUS_INVALID_SYSTEM_SERVICE */
                   "movk x0, #0x001c\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )

/* MYIOSDECK: entered with sp = the frame and x0 = the status (handle_syscall_fault); the TEB
 * from TSD */
__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher_return,
                   IOS_ASM_LOAD_TEB("x11", "x10")
                   "ldr w11, [x11, #0x380]\n\t" /* thread_data->syscall_trace */
                   "cbnz x11, " __ASM_LOCAL_LABEL("trace_syscall_ret") "\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )


/***********************************************************************
 *           __wine_unix_call_dispatcher
 *
 * MYIOSDECK (spec 11 Part A2): upstream's, with the TEB from TSD and x18 neither saved nor
 * restored.
 */
__ASM_GLOBAL_FUNC( __wine_unix_call_dispatcher,
                   "hint 34\n\t" /* bti c */
                   IOS_ASM_LOAD_TEB("x10", "x11")
                   "ldr x10, [x10, #0x378]\n\t" /* thread_data->syscall_frame */
                   "str x19, [x10, #0x98]\n\t"  /* x18 (0x90) is the platform's */
                   "stp x20, x21, [x10, #0xa0]\n\t"
                   "stp x22, x23, [x10, #0xb0]\n\t"
                   "stp x24, x25, [x10, #0xc0]\n\t"
                   "stp x26, x27, [x10, #0xd0]\n\t"
                   "stp x28, x29, [x10, #0xe0]\n\t"
                   "stp q8,  q9,  [x10, #0x1b0]\n\t"
                   "stp q10, q11, [x10, #0x1d0]\n\t"
                   "stp q12, q13, [x10, #0x1f0]\n\t"
                   "stp q14, q15, [x10, #0x210]\n\t"
                   "mov x9, sp\n\t"
                   "stp x30, x9, [x10, #0xf0]\n\t"
                   "mrs x9, NZCV\n\t"
                   "stp x30, x9, [x10, #0x100]\n\t"
                   "mov x19, x10\n\t"
                   /* switch to kernel stack */
                   "mov sp, x10\n\t"
                   /* we're now on the kernel stack, stitch unwind info with previous frame */
                   __ASM_CFI_CFA_IS_AT2(x19, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_CFI(".cfi_offset 29, -0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30, -0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19, -0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20, -0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21, -0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22, -0x98\n\t")
                   __ASM_CFI(".cfi_offset 23, -0x90\n\t")
                   __ASM_CFI(".cfi_offset 24, -0x88\n\t")
                   __ASM_CFI(".cfi_offset 25, -0x80\n\t")
                   __ASM_CFI(".cfi_offset 26, -0x78\n\t")
                   __ASM_CFI(".cfi_offset 27, -0x70\n\t")
                   __ASM_CFI(".cfi_offset 28, -0x68\n\t")
                   "ldr x16, [x0, x1, lsl 3]\n\t"
                   "mov x0, x2\n\t"             /* args */
                   "blr x16\n\t"
                   "ldr w16, [sp, #0x10c]\n\t"  /* frame->restore_flags */
                   "cbnz w16, " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") "\n\t"
                   __ASM_CFI_CFA_IS_AT2(sp, 0x98, 0x02) /* frame->syscall_cfa */
                   "ldr x19, [sp, #0x98]\n\t"   /* x18 is not restored */
                   "ldp x16, x17, [sp, #0xf8]\n\t"
                   /* switch to user stack */
                   "mov sp, x16\n\t"
                   "ret x17" )

#endif  /* __aarch64__ && WINE_IOS */
