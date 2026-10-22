// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08 Part E: the pure rules of thread suspension and contexts, kept apart from the
// server and from Mach so the host tests can run them with fakes (tests/test_ios_suspend.c).
// ios_context.c supplies the real Mach calls; thread.c decides when each rule applies.

#ifndef MYIOSDECK_SERVER_IOS_SUSPEND_H
#define MYIOSDECK_SERVER_IOS_SUSPEND_H

#include <stdint.h>

#include "ios_thread_layout.h"

/* The two Mach calls the rules need, on a thread port name of the app's own task.
 * Each returns 0 (KERN_SUCCESS) or a Mach error. */
struct ios_mach_ops
{
    int (*suspend)( unsigned int port );   /* thread_suspend */
    int (*resume)( unsigned int port );    /* thread_resume */
};

/* ---- Part E1: the snapshot window ----------------------------------------------------------
 * A snapshot stops the thread for as long as it takes to copy its registers, and nothing
 * else: thread_suspend, read, thread_resume. Mach counts suspensions, so a snapshot nested
 * inside a hold (E2) leaves the hold in place. ios_freeze returns 1 if the thread is stopped;
 * only then may ios_thaw be called, so a failed freeze never resumes (and so never releases a
 * hold it didn't take). */
extern int ios_freeze( const struct ios_mach_ops *ops, unsigned int port );
extern void ios_thaw( const struct ios_mach_ops *ops, unsigned int port );

/* ---- Part E2: the hold -----------------------------------------------------------------------
 * held is 0 or 1 (the thread object starts with 0). With real suspension on, the thread's
 * first suspension takes one Mach hold; nested suspensions only count (upstream's counter);
 * the end of its suspension releases exactly that hold. Without real suspension nothing is
 * held. ios_hold_take returns 1 if it took the hold, 0 if there was nothing to do, -1 if
 * thread_suspend failed (held stays 0, so the release will not resume). */
extern int ios_hold_take( const struct ios_mach_ops *ops, int *held, unsigned int port, int real_suspend );
extern void ios_hold_release( const struct ios_mach_ops *ops, int *held, unsigned int port );

/* ---- Part E1/E3: "inside a system call" and the frame fingerprint ---------------------------
 * A thread is inside a system call when its native sp lies on its kernel stack below its
 * current syscall frame (upstream's is_inside_syscall). The bound is strict: with sp equal to
 * the frame the dispatcher is still storing or already reloading the frame, so a write could
 * be half used. */
extern int ios_in_syscall( uint64_t sp, uint64_t kernel_stack, uint64_t frame );

/* What identifies "the same system call": the frame's address and the return pc, sp, fp and
 * lr stored in it. frame == 0 means the thread was not inside a system call at capture. */
struct ios_frame_mark
{
    uint64_t frame;
    uint64_t pc, sp, fp, lr;
};

extern void ios_frame_mark_set( struct ios_frame_mark *mark, uint64_t frame_addr,
                                const struct ios_syscall_frame *frame );
extern void ios_frame_mark_clear( struct ios_frame_mark *mark );
/* 1 if the thread is still inside the system call the mark was taken in */
extern int ios_frame_mark_match( const struct ios_frame_mark *captured, uint64_t frame_addr,
                                 const struct ios_syscall_frame *frame );

/* ---- Part E3: writing native registers into a frame ---------------------------------------
 * The native ARM64 registers as the server keeps them (struct context_data's arm64 parts).
 * The flags use the server's SERVER_CTX_* values. */
#define IOS_REGS_CONTROL         0x01  /* SERVER_CTX_CONTROL: fp (x29), lr (x30), sp, pc, pstate */
#define IOS_REGS_INTEGER         0x02  /* SERVER_CTX_INTEGER: x0..x28 */
#define IOS_REGS_FLOATING_POINT  0x08  /* SERVER_CTX_FLOATING_POINT: v0..v31, fpcr, fpsr */

struct ios_arm64_regs
{
    uint64_t x[31];                         /* x0..x28, fp, lr */
    uint64_t sp, pc;
    uint32_t pstate;
    struct { uint64_t low, high; } q[32];
    uint32_t fpcr, fpsr;
};

/* Copy the parts named by flags into a copy of the frame, never x18, and mark in
 * restore_flags what the dispatcher's return path must reload, as upstream's
 * NtSetContextThread does on its own thread: control and floating point. Integer registers
 * are written too; x19..x28 are reloaded on every return, x0..x17 stay the call's results. */
extern void ios_frame_apply_native( struct ios_syscall_frame *frame, const struct ios_arm64_regs *regs,
                                    unsigned int flags );

#endif /* MYIOSDECK_SERVER_IOS_SUSPEND_H */
