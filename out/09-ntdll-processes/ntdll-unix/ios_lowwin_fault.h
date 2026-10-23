// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 clarification 1: faults at low windows (spec 10 clarification 1) are serviced, not
// delivered. Used by the Mach exception server (ios_mach_exc.c, before the CPU module) and by
// the SIGSEGV/SIGBUS handler (signal_arm64_ios.c, before an exception is built).

#ifndef MYIOSDECK_IOS_LOWWIN_FAULT_H
#define MYIOSDECK_IOS_LOWWIN_FAULT_H

#include <stdint.h>
#include "ios_sig_pure.h"

#define IOS_LW_TEB_PEB 0x60   /* offsetof(TEB, Peb), checked in signal_arm64_ios.c */

enum ios_lw_result
{
    IOS_LW_NO,        /* not serviced: the fault goes on as an ordinary access violation */
    IOS_LW_DONE,      /* serviced: the state was updated, resume the thread with it */
    IOS_LW_NEED_NS,   /* a SIMD access and ns was NULL: call again with the NEON state */
};

/* A data fault at fault_addr of a thread in state ss (ns: its NEON state, or NULL until a SIMD
 * access needs it), with the PEB of that thread's process. Below 4 GB in a low window: either
 * the base register is moved into the real shared data page (fast path), or the load/store is
 * emulated on the real memory and pc advanced. Never touches memory it hasn't checked, never
 * writes x18; logs each declined (pc, page) once and every 4096th access of a window. */
int ios_lowwin_service( struct ios_darwin_ss *ss, struct ios_darwin_ns *ns, uint64_t fault_addr,
                        const void *peb );

#endif /* MYIOSDECK_IOS_LOWWIN_FAULT_H */
