// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part G3: unwinding that makes no progress (ARM64 ntdll, threads of a WoW64 process).
 * After each unwind step the caller passes the new Pc and Sp; the third step in a row that leaves
 * both unchanged reports a stall. Pure code shared by signal_arm64.c and the host tests.
 */

#ifndef MYIOSDECK_ARM64_UNWIND_STALL_H
#define MYIOSDECK_ARM64_UNWIND_STALL_H

#include <stdint.h>

#define ARM64_STALL_LIMIT 3

struct arm64_stall_guard
{
    int      have;
    uint32_t same;
    uint64_t pc;
    uint64_t sp;
};

static inline void arm64_stall_init( struct arm64_stall_guard *g )
{
    g->have = 0;
    g->same = 0;
    g->pc = g->sp = 0;
}

/* 1 when this step is the third in a row that left pc and sp where they were */
static inline int arm64_stall_step( struct arm64_stall_guard *g, uint64_t pc, uint64_t sp )
{
    if (g->have && pc == g->pc && sp == g->sp)
    {
        if (++g->same >= ARM64_STALL_LIMIT) return 1;
    }
    else g->same = 0;
    g->have = 1;
    g->pc = pc;
    g->sp = sp;
    return 0;
}

#endif /* MYIOSDECK_ARM64_UNWIND_STALL_H */
