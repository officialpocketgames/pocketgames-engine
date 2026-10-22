// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08 Part E: the pure rules declared in ios_suspend.h. No server or Mach headers here:
// the host tests build this file as it is.

#include "ios_suspend.h"

/* ---- Part E1: the snapshot window ---- */

int ios_freeze( const struct ios_mach_ops *ops, unsigned int port )
{
    return ops->suspend( port ) == 0;
}

void ios_thaw( const struct ios_mach_ops *ops, unsigned int port )
{
    ops->resume( port );
}

/* ---- Part E2: the hold ---- */

int ios_hold_take( const struct ios_mach_ops *ops, int *held, unsigned int port, int real_suspend )
{
    if (!real_suspend || *held) return 0;
    if (ops->suspend( port )) return -1;
    *held = 1;
    return 1;
}

void ios_hold_release( const struct ios_mach_ops *ops, int *held, unsigned int port )
{
    if (!*held) return;
    *held = 0;
    ops->resume( port );
}

/* ---- Part E1/E3: inside a system call, and which one ---- */

int ios_in_syscall( uint64_t sp, uint64_t kernel_stack, uint64_t frame )
{
    if (!kernel_stack || !frame || kernel_stack >= frame) return 0;
    return sp >= kernel_stack && sp < frame;
}

void ios_frame_mark_set( struct ios_frame_mark *mark, uint64_t frame_addr, const struct ios_syscall_frame *frame )
{
    mark->frame = frame_addr;
    mark->pc = frame->pc;
    mark->sp = frame->sp;
    mark->fp = frame->fp;
    mark->lr = frame->lr;
}

void ios_frame_mark_clear( struct ios_frame_mark *mark )
{
    mark->frame = mark->pc = mark->sp = mark->fp = mark->lr = 0;
}

int ios_frame_mark_match( const struct ios_frame_mark *captured, uint64_t frame_addr,
                          const struct ios_syscall_frame *frame )
{
    if (!captured->frame) return 0;  /* was not inside a system call at capture */
    return frame_addr == captured->frame &&
           frame->pc == captured->pc && frame->sp == captured->sp &&
           frame->fp == captured->fp && frame->lr == captured->lr;
}

/* ---- Part E3: native registers into a frame ---- */

void ios_frame_apply_native( struct ios_syscall_frame *frame, const struct ios_arm64_regs *regs,
                             unsigned int flags )
{
    int i;

    if (flags & IOS_REGS_INTEGER)
    {
        for (i = 0; i < 29; i++)
            if (i != 18) frame->x[i] = regs->x[i];  /* x18 belongs to the platform */
    }
    if (flags & IOS_REGS_CONTROL)
    {
        frame->fp   = regs->x[29];
        frame->lr   = regs->x[30];
        frame->sp   = regs->sp;
        frame->pc   = regs->pc;
        frame->cpsr = regs->pstate;
        frame->restore_flags |= IOS_FRAME_RESTORE_CONTROL;
    }
    if (flags & IOS_REGS_FLOATING_POINT)
    {
        for (i = 0; i < 32; i++)
        {
            frame->v[i].low  = regs->q[i].low;
            frame->v[i].high = regs->q[i].high;
        }
        frame->fpcr = regs->fpcr;
        frame->fpsr = regs->fpsr;
        frame->restore_flags |= IOS_FRAME_RESTORE_FLOATING_POINT;
    }
}
