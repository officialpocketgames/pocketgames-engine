// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08 Part E: thread contexts through Mach. Every Windows thread is a thread of the app's
// own task, and its unix tid is its Mach thread port name (Part C4). Upstream asks a thread for
// its registers with a signal it handles itself; here the server, from its own thread, stops
// the thread for a moment with thread_suspend, copies what it needs with thread_get_state and
// vm_read_overwrite on mach_task_self() (a bad pointer is an error, not a crash), and lets
// it go with thread_resume. No other process is ever touched.
//
// While a thread is stopped this file only makes Mach calls into buffers on the server's
// stack: no allocation, no logging, no lock. The stopped thread may hold the allocator lock
// or the log's lock, which the server shares with it.
//
// The field-by-field conversions follow the layouts of struct context_data
// (include/wine/server_protocol.h) and of AMD64_CONTEXT / I386_CONTEXT (include/winnt.h) and
// the register meanings upstream's dlls/ntdll/unix/thread.c gives them (context_to_server).

#include "config.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/vm_map.h>  /* vm_read_overwrite, vm_write: <mach/mach_vm.h> is unsupported in the iPhoneOS SDK */
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "file.h"
#include "process.h"
#include "thread.h"
#include "request.h"

#include "ios_thread_layout.h"
#include "ios_suspend.h"

#ifdef WINE_IOS

/* spec 11 clarification 4: the TEB offsets used below must come from spec 11 Part A2's shared
 * header (ios_thread_layout.h takes them from it when winternl.h was included first) */
#ifndef IOS_TEB_KERNEL_STACK
#error "ios_syscall_frame.h (spec 11 Part A2) was not included: include winternl.h before ios_thread_layout.h"
#endif

/* The pc/lr/sp/fp accessors of <mach/arm/thread_status.h> (they hide pointer authentication
 * where it applies); plain fields where an SDK lacks them. */
#ifndef arm_thread_state64_get_pc
#define arm_thread_state64_get_pc(ts) ((ts).__pc)
#define arm_thread_state64_get_lr(ts) ((ts).__lr)
#define arm_thread_state64_get_sp(ts) ((ts).__sp)
#define arm_thread_state64_get_fp(ts) ((ts).__fp)
#endif

/* ---- the Mach calls ---- */

static int mach_suspend( unsigned int port ) { return thread_suspend( (thread_act_t)port ); }
static int mach_resume( unsigned int port )  { return thread_resume( (thread_act_t)port ); }

const struct ios_mach_ops ios_mach = { mach_suspend, mach_resume };

/* copy size bytes at addr (own task) into dst; 1 on success */
static int read_mem( uint64_t addr, void *dst, vm_size_t size )
{
    vm_size_t done = 0;

    if (!addr) return 0;
    if (vm_read_overwrite( mach_task_self(), (vm_address_t)addr, size,
                           (vm_address_t)(uintptr_t)dst, &done ) != KERN_SUCCESS)
        return 0;
    return done == size;
}

static int read_u64( uint64_t addr, uint64_t *value )
{
    return read_mem( addr, value, sizeof(*value) );
}

/* The port of a thread the server may stop: known (init_thread was done), with a TEB, and
 * not the server's own thread (which would never wake up). */
int ios_thread_port( struct thread *thread, unsigned int *port )
{
    if (thread->unix_tid == -1 || !thread->teb) return 0;
    if ((mach_port_t)thread->unix_tid == pthread_mach_thread_np( pthread_self() )) return 0;
    *port = (unsigned int)thread->unix_tid;
    return 1;
}

/* ---- Part E1: the copy taken while the thread is stopped ---- */

struct raw_capture
{
    arm_thread_state64_t     ts;           /* native registers where the thread is now */
    arm_neon_state64_t       ns;
    int                      have_ts, have_ns;
    uint64_t                 frame_addr;   /* TEB: current syscall frame */
    uint64_t                 kernel_stack; /* TEB: bottom of the syscall stack */
    int                      in_syscall;
    struct ios_syscall_frame frame;        /* valid if in_syscall */
    unsigned short           guest;        /* guest machine read below, 0 if none */
    uint64_t                 guest_addr;   /* where the guest CONTEXT is */
    uint64_t                 wow_header;   /* WoW64: where its WOW64_CPURESERVED is */
    USHORT                   wow_flags;    /* WoW64: WOW64_CPURESERVED.Flags */
    union
    {
        AMD64_CONTEXT        amd64;        /* ARM64EC: CHPE_V2_CPU_AREA_INFO.ContextAmd64 */
        I386_CONTEXT         i386;         /* WoW64: after WOW64_CPURESERVED */
    } g;
};

/* Runs with the thread stopped: Mach calls into raw only. */
static void copy_stopped_thread( unsigned int port, uint64_t teb, unsigned short machine, struct raw_capture *raw )
{
    mach_msg_type_number_t count;
    uint64_t ptr;

    count = ARM_THREAD_STATE64_COUNT;
    raw->have_ts = thread_get_state( (thread_act_t)port, ARM_THREAD_STATE64,
                                     (thread_state_t)&raw->ts, &count ) == KERN_SUCCESS;
    count = ARM_NEON_STATE64_COUNT;
    raw->have_ns = thread_get_state( (thread_act_t)port, ARM_NEON_STATE64,
                                     (thread_state_t)&raw->ns, &count ) == KERN_SUCCESS;
    if (!raw->have_ts) return;

    if (read_u64( teb + IOS_TEB64_SYSCALL_FRAME, &raw->frame_addr ) &&
        read_u64( teb + IOS_TEB64_KERNEL_STACK, &raw->kernel_stack ) &&
        ios_in_syscall( arm_thread_state64_get_sp( raw->ts ), raw->kernel_stack, raw->frame_addr ))
        raw->in_syscall = read_mem( raw->frame_addr, &raw->frame, sizeof(raw->frame) );

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_AMD64:  /* ARM64EC: TEB->ChpeV2CpuAreaInfo->ContextAmd64 */
        if (read_u64( teb + IOS_TEB64_CHPE_CPU_AREA, &ptr ) &&
            read_u64( ptr + IOS_CHPE_CONTEXT_AMD64, &ptr ) &&
            read_mem( ptr, &raw->g.amd64, sizeof(raw->g.amd64) ))
        {
            raw->guest = machine;
            raw->guest_addr = ptr;
        }
        break;
    case IMAGE_FILE_MACHINE_I386:   /* WoW64: TEB->TlsSlots[WOW64_TLS_CPURESERVED] */
    {
        USHORT header[2];

        /* clarification 9: the CPU module stores the I386_CONTEXT only when it leaves x86 code;
         * while x86 code runs the block is stale, so it is read only inside a system call */
        if (raw->in_syscall &&
            read_u64( teb + IOS_TEB64_WOW64_CPURESERVED, &ptr ) &&
            read_mem( ptr, header, sizeof(header) ) &&
            header[IOS_WOW64_CPURESERVED_MACHINE / 2] == IMAGE_FILE_MACHINE_I386 &&
            read_mem( ptr + IOS_WOW64_I386_CONTEXT, &raw->g.i386, sizeof(raw->g.i386) ))
        {
            raw->guest = machine;
            raw->guest_addr = ptr + IOS_WOW64_I386_CONTEXT;
            raw->wow_header = ptr;
            raw->wow_flags = header[IOS_WOW64_CPURESERVED_FLAGS / 2];
        }
        break;
    }
    }
}

/* ---- conversions to the server's format ---- */

static void native_from_frame( struct context_data *to, const struct ios_syscall_frame *frame )
{
    int i;

    for (i = 0; i < 29; i++) to->integer.arm64_regs.x[i] = frame->x[i];
    to->integer.arm64_regs.x[29] = frame->fp;
    to->integer.arm64_regs.x[30] = frame->lr;
    to->ctl.arm64_regs.sp     = frame->sp;
    to->ctl.arm64_regs.pc     = frame->pc;
    to->ctl.arm64_regs.pstate = frame->cpsr;
    for (i = 0; i < 32; i++)
    {
        to->fp.arm64_regs.q[i].low  = frame->v[i].low;
        to->fp.arm64_regs.q[i].high = frame->v[i].high;
    }
    to->fp.arm64_regs.fpcr = frame->fpcr;
    to->fp.arm64_regs.fpsr = frame->fpsr;
    to->flags |= SERVER_CTX_CONTROL | SERVER_CTX_INTEGER | SERVER_CTX_FLOATING_POINT;
}

static void native_from_state( struct context_data *to, const arm_thread_state64_t *ts,
                               const arm_neon_state64_t *ns )
{
    int i;

    for (i = 0; i < 29; i++) to->integer.arm64_regs.x[i] = ts->__x[i];
    to->integer.arm64_regs.x[29] = (uint64_t)arm_thread_state64_get_fp( *ts );
    to->integer.arm64_regs.x[30] = (uint64_t)arm_thread_state64_get_lr( *ts );
    to->ctl.arm64_regs.sp     = (uint64_t)arm_thread_state64_get_sp( *ts );
    to->ctl.arm64_regs.pc     = (uint64_t)arm_thread_state64_get_pc( *ts );
    to->ctl.arm64_regs.pstate = ts->__cpsr;
    to->flags |= SERVER_CTX_CONTROL | SERVER_CTX_INTEGER;
    if (!ns) return;
    for (i = 0; i < 32; i++)
    {
        to->fp.arm64_regs.q[i].low  = (uint64_t)ns->__v[i];
        to->fp.arm64_regs.q[i].high = (uint64_t)(ns->__v[i] >> 64);
    }
    to->fp.arm64_regs.fpcr = ns->__fpcr;
    to->fp.arm64_regs.fpsr = ns->__fpsr;
    to->flags |= SERVER_CTX_FLOATING_POINT;
}

/* The CPU area holds the whole guest state, whatever its ContextFlags say. */
static void amd64_to_server( struct context_data *to, const AMD64_CONTEXT *from )
{
    to->ctl.x86_64_regs.rip   = from->Rip;
    to->ctl.x86_64_regs.rsp   = from->Rsp;
    to->ctl.x86_64_regs.cs    = from->SegCs;
    to->ctl.x86_64_regs.ss    = from->SegSs;
    to->ctl.x86_64_regs.flags = from->EFlags;
    to->integer.x86_64_regs.rax = from->Rax;
    to->integer.x86_64_regs.rbx = from->Rbx;
    to->integer.x86_64_regs.rcx = from->Rcx;
    to->integer.x86_64_regs.rdx = from->Rdx;
    to->integer.x86_64_regs.rbp = from->Rbp;
    to->integer.x86_64_regs.rsi = from->Rsi;
    to->integer.x86_64_regs.rdi = from->Rdi;
    to->integer.x86_64_regs.r8  = from->R8;
    to->integer.x86_64_regs.r9  = from->R9;
    to->integer.x86_64_regs.r10 = from->R10;
    to->integer.x86_64_regs.r11 = from->R11;
    to->integer.x86_64_regs.r12 = from->R12;
    to->integer.x86_64_regs.r13 = from->R13;
    to->integer.x86_64_regs.r14 = from->R14;
    to->integer.x86_64_regs.r15 = from->R15;
    to->seg.x86_64_regs.ds = from->SegDs;
    to->seg.x86_64_regs.es = from->SegEs;
    to->seg.x86_64_regs.fs = from->SegFs;
    to->seg.x86_64_regs.gs = from->SegGs;
    /* fpregs is an XSAVE_FORMAT image; the CONTEXT keeps MxCsr outside it too */
    memcpy( to->fp.x86_64_regs.fpregs, &from->FltSave, sizeof(to->fp.x86_64_regs.fpregs) );
    ((XSAVE_FORMAT *)to->fp.x86_64_regs.fpregs)->MxCsr = from->MxCsr;
    to->debug.x86_64_regs.dr0 = from->Dr0;
    to->debug.x86_64_regs.dr1 = from->Dr1;
    to->debug.x86_64_regs.dr2 = from->Dr2;
    to->debug.x86_64_regs.dr3 = from->Dr3;
    to->debug.x86_64_regs.dr6 = from->Dr6;
    to->debug.x86_64_regs.dr7 = from->Dr7;
    to->flags |= SERVER_CTX_CONTROL | SERVER_CTX_INTEGER | SERVER_CTX_SEGMENTS |
                 SERVER_CTX_FLOATING_POINT | SERVER_CTX_DEBUG_REGISTERS;
}

static void i386_to_server( struct context_data *to, const I386_CONTEXT *from )
{
    to->ctl.i386_regs.eip    = from->Eip;
    to->ctl.i386_regs.ebp    = from->Ebp;
    to->ctl.i386_regs.esp    = from->Esp;
    to->ctl.i386_regs.eflags = from->EFlags;
    to->ctl.i386_regs.cs     = from->SegCs;
    to->ctl.i386_regs.ss     = from->SegSs;
    to->integer.i386_regs.eax = from->Eax;
    to->integer.i386_regs.ebx = from->Ebx;
    to->integer.i386_regs.ecx = from->Ecx;
    to->integer.i386_regs.edx = from->Edx;
    to->integer.i386_regs.esi = from->Esi;
    to->integer.i386_regs.edi = from->Edi;
    to->seg.i386_regs.ds = from->SegDs;
    to->seg.i386_regs.es = from->SegEs;
    to->seg.i386_regs.fs = from->SegFs;
    to->seg.i386_regs.gs = from->SegGs;
    to->fp.i386_regs.ctrl     = from->FloatSave.ControlWord;
    to->fp.i386_regs.status   = from->FloatSave.StatusWord;
    to->fp.i386_regs.tag      = from->FloatSave.TagWord;
    to->fp.i386_regs.err_off  = from->FloatSave.ErrorOffset;
    to->fp.i386_regs.err_sel  = from->FloatSave.ErrorSelector;
    to->fp.i386_regs.data_off = from->FloatSave.DataOffset;
    to->fp.i386_regs.data_sel = from->FloatSave.DataSelector;
    to->fp.i386_regs.cr0npx   = from->FloatSave.Cr0NpxState;
    memcpy( to->fp.i386_regs.regs, from->FloatSave.RegisterArea, sizeof(to->fp.i386_regs.regs) );
    to->debug.i386_regs.dr0 = from->Dr0;
    to->debug.i386_regs.dr1 = from->Dr1;
    to->debug.i386_regs.dr2 = from->Dr2;
    to->debug.i386_regs.dr3 = from->Dr3;
    to->debug.i386_regs.dr6 = from->Dr6;
    to->debug.i386_regs.dr7 = from->Dr7;
    memcpy( to->ext.i386_regs, from->ExtendedRegisters, sizeof(to->ext.i386_regs) );
    to->flags |= SERVER_CTX_CONTROL | SERVER_CTX_INTEGER | SERVER_CTX_SEGMENTS |
                 SERVER_CTX_FLOATING_POINT | SERVER_CTX_DEBUG_REGISTERS | SERVER_CTX_EXTENDED_REGISTERS;
}

/* Part E1: fill regs[0] (native, ARM64) and regs[1] (guest, if the process's machine is not
 * the native one) from the thread as it is now, and record in mark which system call it is
 * in (frame 0 if none). The registers Windows sees for a thread inside a system call are the
 * ones its syscall frame saved at entry; otherwise its current Mach thread state. x18 is
 * reported as the TEB address, its meaning on Windows. Returns a status. */
unsigned int ios_capture_context( struct thread *thread, struct context_data regs[2], struct ios_frame_mark *mark )
{
    struct raw_capture raw;
    unsigned short machine = thread->process->machine;
    unsigned int port;

    ios_frame_mark_clear( mark );
    if (!ios_thread_port( thread, &port )) return STATUS_UNSUCCESSFUL;

    memset( &raw, 0, sizeof(raw) );
    if (!ios_freeze( &ios_mach, port ))
    {
        fprintf( stderr, "wineserver: %04x: can't stop thread port %#x to read its context\n",
                 thread->id, port );
        return STATUS_UNSUCCESSFUL;
    }
    copy_stopped_thread( port, thread->teb, machine, &raw );
    ios_thaw( &ios_mach, port );
    if (!raw.have_ts) return STATUS_UNSUCCESSFUL;

    memset( regs, 0, 2 * sizeof(*regs) );
    regs[0].machine = native_machine;
    if (raw.in_syscall)
    {
        native_from_frame( &regs[0], &raw.frame );
        ios_frame_mark_set( mark, raw.frame_addr, &raw.frame );
    }
    else native_from_state( &regs[0], &raw.ts, raw.have_ns ? &raw.ns : NULL );
    regs[0].integer.arm64_regs.x[18] = thread->teb;
    regs[0].exec_space.space.space = raw.in_syscall ? EXEC_SPACE_SYSCALL : EXEC_SPACE_USERMODE;
    regs[0].flags |= SERVER_CTX_EXEC_SPACE;

    /* the guest half always carries the machine; with nothing valid (clarification 9: a WoW64
     * thread outside a system call) it has no flags */
    if (machine != native_machine)
    {
        regs[1].machine = machine;
        if (raw.guest == IMAGE_FILE_MACHINE_AMD64) amd64_to_server( &regs[1], &raw.g.amd64 );
        else if (raw.guest == IMAGE_FILE_MACHINE_I386) i386_to_server( &regs[1], &raw.g.i386 );
    }
    return STATUS_SUCCESS;
}

/* ---- Part E3: writes ---- */

static kern_return_t write_mem( uint64_t addr, const void *src, size_t size )
{
    return vm_write( mach_task_self(), (vm_address_t)addr, (vm_offset_t)(uintptr_t)src,
                     (mach_msg_type_number_t)size );
}

/* the writable parts of a frame: never x18, never the dispatcher's own bookkeeping */
static kern_return_t write_frame( uint64_t addr, const struct ios_syscall_frame *frame )
{
    static const struct { unsigned int start, end; } parts[] =
    {
        { IOS_FRAME_WRITE1_START, IOS_FRAME_WRITE1_END },
        { IOS_FRAME_WRITE2_START, IOS_FRAME_WRITE2_END },
        { IOS_FRAME_WRITE3_START, IOS_FRAME_WRITE3_END },
    };
    kern_return_t kr = KERN_SUCCESS;
    unsigned int i;

    for (i = 0; i < sizeof(parts) / sizeof(parts[0]) && kr == KERN_SUCCESS; i++)
        kr = write_mem( addr + parts[i].start, (const char *)frame + parts[i].start,
                        parts[i].end - parts[i].start );
    return kr;
}

static void native_regs_from_server( struct ios_arm64_regs *to, const struct context_data *from )
{
    int i;

    for (i = 0; i < 31; i++) to->x[i] = from->integer.arm64_regs.x[i];
    to->sp     = from->ctl.arm64_regs.sp;
    to->pc     = from->ctl.arm64_regs.pc;
    to->pstate = (uint32_t)from->ctl.arm64_regs.pstate;
    for (i = 0; i < 32; i++)
    {
        to->q[i].low  = from->fp.arm64_regs.q[i].low;
        to->q[i].high = from->fp.arm64_regs.q[i].high;
    }
    to->fpcr = from->fp.arm64_regs.fpcr;
    to->fpsr = from->fp.arm64_regs.fpsr;
}

/* the inverse of amd64_to_server, for the parts named by flags */
static void amd64_from_server( AMD64_CONTEXT *to, const struct context_data *from, unsigned int flags )
{
    if (flags & SERVER_CTX_CONTROL)
    {
        to->Rip    = from->ctl.x86_64_regs.rip;
        to->Rsp    = from->ctl.x86_64_regs.rsp;
        to->SegCs  = from->ctl.x86_64_regs.cs;
        to->SegSs  = from->ctl.x86_64_regs.ss;
        to->EFlags = from->ctl.x86_64_regs.flags;
    }
    if (flags & SERVER_CTX_INTEGER)
    {
        to->Rax = from->integer.x86_64_regs.rax;
        to->Rbx = from->integer.x86_64_regs.rbx;
        to->Rcx = from->integer.x86_64_regs.rcx;
        to->Rdx = from->integer.x86_64_regs.rdx;
        to->Rbp = from->integer.x86_64_regs.rbp;
        to->Rsi = from->integer.x86_64_regs.rsi;
        to->Rdi = from->integer.x86_64_regs.rdi;
        to->R8  = from->integer.x86_64_regs.r8;
        to->R9  = from->integer.x86_64_regs.r9;
        to->R10 = from->integer.x86_64_regs.r10;
        to->R11 = from->integer.x86_64_regs.r11;
        to->R12 = from->integer.x86_64_regs.r12;
        to->R13 = from->integer.x86_64_regs.r13;
        to->R14 = from->integer.x86_64_regs.r14;
        to->R15 = from->integer.x86_64_regs.r15;
    }
    if (flags & SERVER_CTX_SEGMENTS)
    {
        to->SegDs = from->seg.x86_64_regs.ds;
        to->SegEs = from->seg.x86_64_regs.es;
        to->SegFs = from->seg.x86_64_regs.fs;
        to->SegGs = from->seg.x86_64_regs.gs;
    }
    if (flags & SERVER_CTX_FLOATING_POINT)
    {
        memcpy( &to->FltSave, from->fp.x86_64_regs.fpregs, sizeof(to->FltSave) );
        to->MxCsr = to->FltSave.MxCsr;
    }
    if (flags & SERVER_CTX_DEBUG_REGISTERS)
    {
        to->Dr0 = from->debug.x86_64_regs.dr0;
        to->Dr1 = from->debug.x86_64_regs.dr1;
        to->Dr2 = from->debug.x86_64_regs.dr2;
        to->Dr3 = from->debug.x86_64_regs.dr3;
        to->Dr6 = from->debug.x86_64_regs.dr6;
        to->Dr7 = from->debug.x86_64_regs.dr7;
    }
}

/* the inverse of i386_to_server, for the parts named by flags */
static void i386_from_server( I386_CONTEXT *to, const struct context_data *from, unsigned int flags )
{
    if (flags & SERVER_CTX_CONTROL)
    {
        to->Eip    = from->ctl.i386_regs.eip;
        to->Ebp    = from->ctl.i386_regs.ebp;
        to->Esp    = from->ctl.i386_regs.esp;
        to->EFlags = from->ctl.i386_regs.eflags;
        to->SegCs  = from->ctl.i386_regs.cs;
        to->SegSs  = from->ctl.i386_regs.ss;
    }
    if (flags & SERVER_CTX_INTEGER)
    {
        to->Eax = from->integer.i386_regs.eax;
        to->Ebx = from->integer.i386_regs.ebx;
        to->Ecx = from->integer.i386_regs.ecx;
        to->Edx = from->integer.i386_regs.edx;
        to->Esi = from->integer.i386_regs.esi;
        to->Edi = from->integer.i386_regs.edi;
    }
    if (flags & SERVER_CTX_SEGMENTS)
    {
        to->SegDs = from->seg.i386_regs.ds;
        to->SegEs = from->seg.i386_regs.es;
        to->SegFs = from->seg.i386_regs.fs;
        to->SegGs = from->seg.i386_regs.gs;
    }
    if (flags & SERVER_CTX_FLOATING_POINT)
    {
        to->FloatSave.ControlWord   = from->fp.i386_regs.ctrl;
        to->FloatSave.StatusWord    = from->fp.i386_regs.status;
        to->FloatSave.TagWord       = from->fp.i386_regs.tag;
        to->FloatSave.ErrorOffset   = from->fp.i386_regs.err_off;
        to->FloatSave.ErrorSelector = from->fp.i386_regs.err_sel;
        to->FloatSave.DataOffset    = from->fp.i386_regs.data_off;
        to->FloatSave.DataSelector  = from->fp.i386_regs.data_sel;
        to->FloatSave.Cr0NpxState   = from->fp.i386_regs.cr0npx;
        memcpy( to->FloatSave.RegisterArea, from->fp.i386_regs.regs, sizeof(to->FloatSave.RegisterArea) );
    }
    if (flags & SERVER_CTX_DEBUG_REGISTERS)
    {
        to->Dr0 = from->debug.i386_regs.dr0;
        to->Dr1 = from->debug.i386_regs.dr1;
        to->Dr2 = from->debug.i386_regs.dr2;
        to->Dr3 = from->debug.i386_regs.dr3;
        to->Dr6 = from->debug.i386_regs.dr6;
        to->Dr7 = from->debug.i386_regs.dr7;
    }
    if (flags & SERVER_CTX_EXTENDED_REGISTERS)
        memcpy( to->ExtendedRegisters, from->ext.i386_regs, sizeof(to->ExtendedRegisters) );
}

#define NATIVE_WRITABLE (SERVER_CTX_CONTROL | SERVER_CTX_INTEGER | SERVER_CTX_FLOATING_POINT)
#define GUEST_WRITABLE  (SERVER_CTX_CONTROL | SERVER_CTX_INTEGER | SERVER_CTX_SEGMENTS | \
                         SERVER_CTX_FLOATING_POINT | SERVER_CTX_DEBUG_REGISTERS | SERVER_CTX_EXTENDED_REGISTERS)

/* Part E3: apply the registers SetThreadContext changed (dirty[0] native, dirty[1] guest,
 * SERVER_CTX_* flags) when the thread resumes. Only if it is still inside the system call
 * mark was taken in: same frame address, same return pc/sp/fp/lr. Then the native registers
 * go into its syscall frame (never x18) and the guest registers into its CPU area; a WoW64
 * thread also gets WOW64_CPURESERVED_FLAG_RESET_STATE so its CPU module reloads them.
 * Anything else is dropped: registers from one system call must never be written into
 * another. Returns NULL when applied (or when there was nothing to apply), else why it was
 * dropped, for the caller to log once the thread is free to run (a held thread may own the
 * log's lock). */
const char *ios_apply_context( struct thread *thread, const struct context_data regs[2],
                               const unsigned int dirty[2], const struct ios_frame_mark *mark )
{
    unsigned short machine = thread->process->machine;
    unsigned int native = dirty[0] & NATIVE_WRITABLE;
    unsigned int guest = (regs[1].machine && regs[1].machine == machine) ? dirty[1] & GUEST_WRITABLE : 0;
    struct ios_arm64_regs native_regs;
    struct raw_capture raw;
    const char *drop = NULL;
    unsigned int port;
    kern_return_t kr = KERN_SUCCESS;

    if (!native && !guest) return NULL;
    if (native) native_regs_from_server( &native_regs, &regs[0] );
    memset( &raw, 0, sizeof(raw) );

    if (!mark->frame) drop = "it was not inside a system call when its context was read";
    else if (!ios_thread_port( thread, &port )) drop = "it has no thread port";
    else if (!ios_freeze( &ios_mach, port )) drop = "it can't be stopped";
    else
    {
        /* with the thread stopped: Mach calls into buffers on this stack only */
        copy_stopped_thread( port, thread->teb, guest ? machine : 0, &raw );
        if (!raw.in_syscall || !ios_frame_mark_match( mark, raw.frame_addr, &raw.frame ))
            drop = "it has left the system call it was in";
        else if (guest && raw.guest != machine)
            drop = "its CPU area can't be read";
        else
        {
            if (native)
            {
                ios_frame_apply_native( &raw.frame, &native_regs, native );
                kr = write_frame( raw.frame_addr, &raw.frame );
            }
            if (kr == KERN_SUCCESS && guest)  /* raw.guest is AMD64 or I386 here */
            {
                if (machine == IMAGE_FILE_MACHINE_AMD64)
                {
                    amd64_from_server( &raw.g.amd64, &regs[1], guest );
                    kr = write_mem( raw.guest_addr, &raw.g.amd64, sizeof(raw.g.amd64) );
                }
                else
                {
                    i386_from_server( &raw.g.i386, &regs[1], guest );
                    kr = write_mem( raw.guest_addr, &raw.g.i386, sizeof(raw.g.i386) );
                    raw.wow_flags |= IOS_WOW64_RESET_STATE;
                    if (kr == KERN_SUCCESS)
                        kr = write_mem( raw.wow_header + IOS_WOW64_CPURESERVED_FLAGS, &raw.wow_flags,
                                        sizeof(raw.wow_flags) );
                }
            }
            if (kr != KERN_SUCCESS) drop = "writing its memory failed";
        }
        ios_thaw( &ios_mach, port );
    }
    return drop;
}

#endif /* WINE_IOS */
