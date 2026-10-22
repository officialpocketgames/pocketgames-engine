// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part A2: the arm64 system-call frame and the TEB fields that point at it, shared by
// signal_arm64_ios.c (whose assembly uses the numbers below) and spec 08 Part E (which reads a
// thread's frame to capture its context). The layout is upstream Wine 11.4's; the asserts keep
// the numbers, the struct and the assembly in step. Include it after the Wine headers
// (ULONG64, NEON128); signal_arm64_ios.c checks the TEB offsets against struct
// ntdll_thread_data.

#ifndef MYIOSDECK_IOS_SYSCALL_FRAME_H
#define MYIOSDECK_IOS_SYSCALL_FRAME_H

#include <stddef.h>

/* offsets in struct syscall_frame */
#define IOS_FRAME_X              0x000   /* x0-x28 (x18's slot is never saved or restored) */
#define IOS_FRAME_FP             0x0e8
#define IOS_FRAME_LR             0x0f0
#define IOS_FRAME_SP             0x0f8
#define IOS_FRAME_PC             0x100
#define IOS_FRAME_CPSR           0x108
#define IOS_FRAME_RESTORE_FLAGS  0x10c
#define IOS_FRAME_PREV_FRAME     0x110
#define IOS_FRAME_SYSCALL_CFA    0x118
#define IOS_FRAME_SYSCALL_ID     0x120
#define IOS_FRAME_FPCR           0x128
#define IOS_FRAME_FPSR           0x12c
#define IOS_FRAME_V              0x130
#define IOS_FRAME_SIZE           0x330

/* offsets in the 64-bit TEB (struct ntdll_thread_data lives in GdiTebBatch) */
#define IOS_TEB_SYSCALL_TABLE    0x370
#define IOS_TEB_SYSCALL_FRAME    0x378
#define IOS_TEB_SYSCALL_TRACE    0x380
#define IOS_TEB_KERNEL_STACK     0x3a8   /* the kernel stack's base (lowest address) */
#define IOS_KERNEL_STACK_SIZE    0x100000 /* = kernel_stack_size; the frame sits at its top */
#define IOS_TEB_PEB              0x060   /* TEB64.Peb */

struct syscall_frame
{
    ULONG64               x[29];          /* 000 */
    ULONG64               fp;             /* 0e8 */
    ULONG64               lr;             /* 0f0 */
    ULONG64               sp;             /* 0f8 */
    ULONG64               pc;             /* 100 */
    ULONG                 cpsr;           /* 108 */
    ULONG                 restore_flags;  /* 10c */
    struct syscall_frame *prev_frame;     /* 110 */
    void                 *syscall_cfa;    /* 118 */
    ULONG                 syscall_id;     /* 120 */
    ULONG                 align;          /* 124 */
    ULONG                 fpcr;           /* 128 */
    ULONG                 fpsr;           /* 12c */
    NEON128               v[32];          /* 130 */
};

_Static_assert( offsetof( struct syscall_frame, x ) == IOS_FRAME_X, "frame x" );
_Static_assert( offsetof( struct syscall_frame, fp ) == IOS_FRAME_FP, "frame fp" );
_Static_assert( offsetof( struct syscall_frame, lr ) == IOS_FRAME_LR, "frame lr" );
_Static_assert( offsetof( struct syscall_frame, sp ) == IOS_FRAME_SP, "frame sp" );
_Static_assert( offsetof( struct syscall_frame, pc ) == IOS_FRAME_PC, "frame pc" );
_Static_assert( offsetof( struct syscall_frame, cpsr ) == IOS_FRAME_CPSR, "frame cpsr" );
_Static_assert( offsetof( struct syscall_frame, restore_flags ) == IOS_FRAME_RESTORE_FLAGS, "frame restore_flags" );
_Static_assert( offsetof( struct syscall_frame, prev_frame ) == IOS_FRAME_PREV_FRAME, "frame prev_frame" );
_Static_assert( offsetof( struct syscall_frame, syscall_cfa ) == IOS_FRAME_SYSCALL_CFA, "frame syscall_cfa" );
_Static_assert( offsetof( struct syscall_frame, syscall_id ) == IOS_FRAME_SYSCALL_ID, "frame syscall_id" );
_Static_assert( offsetof( struct syscall_frame, fpcr ) == IOS_FRAME_FPCR, "frame fpcr" );
_Static_assert( offsetof( struct syscall_frame, fpsr ) == IOS_FRAME_FPSR, "frame fpsr" );
_Static_assert( offsetof( struct syscall_frame, v ) == IOS_FRAME_V, "frame v" );
_Static_assert( sizeof( struct syscall_frame ) == IOS_FRAME_SIZE, "frame size" );

#endif /* MYIOSDECK_IOS_SYSCALL_FRAME_H */
