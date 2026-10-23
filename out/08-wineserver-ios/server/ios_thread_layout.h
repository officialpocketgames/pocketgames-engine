// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08 Part E: where a client thread keeps the state the server reads and writes through
// Mach (its TEB, its current syscall frame, its guest CPU area). All addresses are in the
// app's own task (every pseudo-process is threads of the app).
//
// The numbers follow upstream Wine 11.4: include/winternl.h (TEB64, CHPE_V2_CPU_AREA_INFO,
// WOW64_CPURESERVED), dlls/ntdll/unix/unix_private.h (struct ntdll_thread_data, stored at
// TEB64.GdiTebBatch) and dlls/ntdll/unix/signal_arm64.c (struct syscall_frame).
//
// In the server build (winternl.h included first) the syscall-frame TEB offsets come from spec
// 11 Part A2's shared header, ios_syscall_frame.h (spec 11 clarification 4; build-ios.sh copies
// it from out/09-ntdll-processes/ntdll-unix/), and every number here is checked against it and
// against Wine's declarations. The host tests have no Wine headers: they get the same numbers
// written out, which the server build proves equal.

#ifndef MYIOSDECK_SERVER_IOS_THREAD_LAYOUT_H
#define MYIOSDECK_SERVER_IOS_THREAD_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

/* ---- the 64-bit TEB ---------------------------------------------------------------------- */

/* TEB64.GdiTebBatch: ntdll's unix side keeps struct ntdll_thread_data there */
#define IOS_TEB64_THREAD_DATA        0x2f0
#ifdef __WINE_WINTERNL_H
#include "ios_syscall_frame.h"   /* spec 11 Part A2 */
/* ntdll_thread_data.syscall_frame: the thread's current (innermost) syscall frame */
#define IOS_TEB64_SYSCALL_FRAME      IOS_TEB_SYSCALL_FRAME
/* ntdll_thread_data.kernel_stack: lowest address of the stack syscalls run on */
#define IOS_TEB64_KERNEL_STACK       IOS_TEB_KERNEL_STACK
#else  /* host tests: the same numbers, proved equal in the server build */
#define IOS_TEB64_SYSCALL_FRAME      0x378
#define IOS_TEB64_KERNEL_STACK       0x3a8
#endif
/* TEB64.TlsSlots[64] */
#define IOS_TEB64_TLS_SLOTS          0x1480
/* TEB64.ChpeV2CpuAreaInfo (ARM64EC processes) */
#define IOS_TEB64_CHPE_CPU_AREA      0x1788

/* TlsSlots[WOW64_TLS_CPURESERVED] of a WoW64 thread points at its WOW64_CPURESERVED */
#define IOS_WOW64_TLS_CPURESERVED    1
#define IOS_TEB64_WOW64_CPURESERVED  (IOS_TEB64_TLS_SLOTS + 8 * IOS_WOW64_TLS_CPURESERVED)

/* ---- the guest CPU areas ------------------------------------------------------------------ */

/* CHPE_V2_CPU_AREA_INFO.ContextAmd64: pointer to the thread's x64 CONTEXT (ARM64EC) */
#define IOS_CHPE_CONTEXT_AMD64       0x18

/* WOW64_CPURESERVED is { USHORT Flags; USHORT Machine; } followed by the guest CONTEXT,
 * aligned to that CONTEXT's alignment (4 for I386_CONTEXT, RtlWow64GetCpuAreaInfo). */
#define IOS_WOW64_CPURESERVED_FLAGS   0
#define IOS_WOW64_CPURESERVED_MACHINE 2
#define IOS_WOW64_I386_CONTEXT        4
#define IOS_WOW64_RESET_STATE         1    /* WOW64_CPURESERVED_FLAG_RESET_STATE */

#define IOS_AMD64_CONTEXT_SIZE       0x4d0 /* sizeof(AMD64_CONTEXT), same as ARM64EC_NT_CONTEXT */
#define IOS_I386_CONTEXT_SIZE        0x2cc /* sizeof(I386_CONTEXT) */

/* ---- the syscall frame (upstream signal_arm64.c, struct syscall_frame) ------------------- */

struct ios_syscall_frame
{
    uint64_t x[29];                         /* 000 */
    uint64_t fp;                            /* 0e8 */
    uint64_t lr;                            /* 0f0 */
    uint64_t sp;                            /* 0f8 */
    uint64_t pc;                            /* 100 */
    uint32_t cpsr;                          /* 108 */
    uint32_t restore_flags;                 /* 10c */
    uint64_t prev_frame;                    /* 110 */
    uint64_t syscall_cfa;                   /* 118 */
    uint32_t syscall_id;                    /* 120 */
    uint32_t align;                         /* 124 */
    uint32_t fpcr;                          /* 128 */
    uint32_t fpsr;                          /* 12c */
    struct { uint64_t low, high; } v[32];   /* 130 */
};

_Static_assert( offsetof(struct ios_syscall_frame, fp) == 0xe8, "syscall_frame.fp" );
_Static_assert( offsetof(struct ios_syscall_frame, lr) == 0xf0, "syscall_frame.lr" );
_Static_assert( offsetof(struct ios_syscall_frame, sp) == 0xf8, "syscall_frame.sp" );
_Static_assert( offsetof(struct ios_syscall_frame, pc) == 0x100, "syscall_frame.pc" );
_Static_assert( offsetof(struct ios_syscall_frame, cpsr) == 0x108, "syscall_frame.cpsr" );
_Static_assert( offsetof(struct ios_syscall_frame, restore_flags) == 0x10c, "syscall_frame.restore_flags" );
_Static_assert( offsetof(struct ios_syscall_frame, prev_frame) == 0x110, "syscall_frame.prev_frame" );
_Static_assert( offsetof(struct ios_syscall_frame, syscall_id) == 0x120, "syscall_frame.syscall_id" );
_Static_assert( offsetof(struct ios_syscall_frame, fpcr) == 0x128, "syscall_frame.fpcr" );
_Static_assert( offsetof(struct ios_syscall_frame, fpsr) == 0x12c, "syscall_frame.fpsr" );
_Static_assert( offsetof(struct ios_syscall_frame, v) == 0x130, "syscall_frame.v" );
_Static_assert( sizeof(struct ios_syscall_frame) == 0x330, "syscall_frame size" );

/* The parts of a frame the server may write (Part E3). x18 (0x090..0x098) is never written,
 * nor the dispatcher's own fields (prev_frame, syscall_cfa, syscall_id, align). */
#define IOS_FRAME_X18                 0x90
#define IOS_FRAME_WRITE1_START        0x000                 /* x0..x17 */
#define IOS_FRAME_WRITE1_END          IOS_FRAME_X18
#define IOS_FRAME_WRITE2_START        (IOS_FRAME_X18 + 8)   /* x19..x28, fp, lr, sp, pc, cpsr, restore_flags */
#define IOS_FRAME_WRITE2_END          0x110
#define IOS_FRAME_WRITE3_START        0x128                 /* fpcr, fpsr, v0..v31 */
#define IOS_FRAME_WRITE3_END          0x330

/* frame->restore_flags bits read by the dispatcher's return path: upstream stores the ARM64
 * CONTEXT_* flags without CONTEXT_ARM64 (CONTROL 0x1, INTEGER 0x2, FLOATING_POINT 0x4) */
#define IOS_FRAME_RESTORE_CONTROL        0x1
#define IOS_FRAME_RESTORE_INTEGER        0x2
#define IOS_FRAME_RESTORE_FLOATING_POINT 0x4

/* ---- cross-checks against Wine's headers (server build) ----------------------------------- */

#ifdef __WINE_WINTERNL_H
/* this file's frame copy against spec 11's shared header */
_Static_assert( IOS_TEB64_SYSCALL_FRAME == 0x378 && IOS_TEB64_KERNEL_STACK == 0x3a8, "host-test copy of the TEB offsets" );
_Static_assert( offsetof(struct ios_syscall_frame, fp) == IOS_FRAME_FP, "shared header: fp" );
_Static_assert( offsetof(struct ios_syscall_frame, lr) == IOS_FRAME_LR, "shared header: lr" );
_Static_assert( offsetof(struct ios_syscall_frame, sp) == IOS_FRAME_SP, "shared header: sp" );
_Static_assert( offsetof(struct ios_syscall_frame, pc) == IOS_FRAME_PC, "shared header: pc" );
_Static_assert( offsetof(struct ios_syscall_frame, cpsr) == IOS_FRAME_CPSR, "shared header: cpsr" );
_Static_assert( offsetof(struct ios_syscall_frame, restore_flags) == IOS_FRAME_RESTORE_FLAGS, "shared header: restore_flags" );
_Static_assert( offsetof(struct ios_syscall_frame, prev_frame) == IOS_FRAME_PREV_FRAME, "shared header: prev_frame" );
_Static_assert( offsetof(struct ios_syscall_frame, syscall_cfa) == IOS_FRAME_SYSCALL_CFA, "shared header: syscall_cfa" );
_Static_assert( offsetof(struct ios_syscall_frame, syscall_id) == IOS_FRAME_SYSCALL_ID, "shared header: syscall_id" );
_Static_assert( offsetof(struct ios_syscall_frame, fpcr) == IOS_FRAME_FPCR, "shared header: fpcr" );
_Static_assert( offsetof(struct ios_syscall_frame, fpsr) == IOS_FRAME_FPSR, "shared header: fpsr" );
_Static_assert( offsetof(struct ios_syscall_frame, v) == IOS_FRAME_V, "shared header: v" );
_Static_assert( sizeof(struct ios_syscall_frame) == IOS_FRAME_SIZE, "shared header: size" );
_Static_assert( sizeof(struct ios_syscall_frame) == sizeof(struct syscall_frame), "shared header: struct size" );

_Static_assert( offsetof(TEB64, GdiTebBatch) == IOS_TEB64_THREAD_DATA, "TEB64.GdiTebBatch" );
_Static_assert( offsetof(TEB64, TlsSlots) == IOS_TEB64_TLS_SLOTS, "TEB64.TlsSlots" );
_Static_assert( offsetof(TEB64, ChpeV2CpuAreaInfo) == IOS_TEB64_CHPE_CPU_AREA, "TEB64.ChpeV2CpuAreaInfo" );
_Static_assert( WOW64_TLS_CPURESERVED == IOS_WOW64_TLS_CPURESERVED, "WOW64_TLS_CPURESERVED" );
_Static_assert( WOW64_CPURESERVED_FLAG_RESET_STATE == IOS_WOW64_RESET_STATE, "RESET_STATE flag" );
_Static_assert( offsetof(WOW64_CPURESERVED, Flags) == IOS_WOW64_CPURESERVED_FLAGS, "WOW64_CPURESERVED.Flags" );
_Static_assert( offsetof(WOW64_CPURESERVED, Machine) == IOS_WOW64_CPURESERVED_MACHINE, "WOW64_CPURESERVED.Machine" );
_Static_assert( sizeof(WOW64_CPURESERVED) == IOS_WOW64_I386_CONTEXT, "I386_CONTEXT follows the header" );
_Static_assert( offsetof(CHPE_V2_CPU_AREA_INFO, ContextAmd64) == IOS_CHPE_CONTEXT_AMD64, "ContextAmd64" );
_Static_assert( sizeof(AMD64_CONTEXT) == IOS_AMD64_CONTEXT_SIZE, "AMD64_CONTEXT size" );
_Static_assert( sizeof(ARM64EC_NT_CONTEXT) == IOS_AMD64_CONTEXT_SIZE, "ARM64EC_NT_CONTEXT size" );
_Static_assert( sizeof(I386_CONTEXT) == IOS_I386_CONTEXT_SIZE, "I386_CONTEXT size" );
#endif

#endif /* MYIOSDECK_SERVER_IOS_THREAD_LAYOUT_H */
