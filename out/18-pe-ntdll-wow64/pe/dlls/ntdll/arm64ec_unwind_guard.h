// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part F: frame checks for exception dispatch and unwinding of x64 code (ARM64EC).
 *
 * Pure code shared by signal_arm64ec.c and the host tests. Memory queries come in through a
 * callback so the tests can fake a region list.
 *
 *  F1  the stack window of one walk (the allocation holding Rsp) and its step budget;
 *  F2  frame validity inside a window;
 *  F3  phase-one rules: end of stack, no progress / livelock, terminal frame, first-step bad frame;
 *  F4  phase-two step guard;
 *  F5  coverage of the ARM64EC code bitmap.
 *
 * The window is state of one walk: callers keep it on their own stack. Nothing here is a global
 * except the process-wide livelock record, which the caller owns and passes in.
 */

#ifndef MYIOSDECK_ARM64EC_UNWIND_GUARD_H
#define MYIOSDECK_ARM64EC_UNWIND_GUARD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct ec_region
{
    uint64_t base;        /* BaseAddress */
    uint64_t alloc_base;  /* AllocationBase */
    uint64_t size;        /* RegionSize */
    int      committed;   /* State == MEM_COMMIT */
};

/* 1 and *region filled on success */
typedef int (*ec_query_fn)( void *ctx, uint64_t addr, struct ec_region *region );

#define EC_WINDOW_MAX_REGIONS  4096
#define EC_BUDGET_MIN          256
#define EC_BUDGET_MAX          65536
#define EC_PHASE1_MAX_STEPS    65536
#define EC_LIVELOCK_LIMIT      16
#define EC_UNWIND_REPEAT_LIMIT 4

struct ec_stack_window
{
    int      valid;        /* 0: Rsp was not committed, no window */
    uint64_t low;          /* window is [low, high) */
    uint64_t high;
    uint64_t emu_low;      /* emulator stack [emu_low, emu_high], both 0 when none */
    uint64_t emu_high;
    uint32_t budget;       /* phase-two step budget */
};

static inline uint32_t ec_window_budget( uint64_t size )
{
    uint64_t steps = size / 16;
    if (steps < EC_BUDGET_MIN) steps = EC_BUDGET_MIN;
    if (steps > EC_BUDGET_MAX) steps = EC_BUDGET_MAX;
    return (uint32_t)steps;
}

/* F1: establish the window from rsp; emu_low/emu_high come from the CPU area (0, 0 if none) */
static inline void ec_window_init( struct ec_stack_window *w, uint64_t rsp, ec_query_fn query, void *ctx,
                                   uint64_t emu_low, uint64_t emu_high )
{
    struct ec_region region;
    uint64_t addr, alloc;
    unsigned int i;

    memset( w, 0, sizeof(*w) );
    w->emu_low = emu_low;
    w->emu_high = emu_high;
    w->budget = EC_BUDGET_MIN;
    if (!query( ctx, rsp, &region ) || !region.committed) return;

    alloc = region.alloc_base;
    w->low = w->high = alloc;
    for (addr = alloc, i = 0; i < EC_WINDOW_MAX_REGIONS; i++)
    {
        if (!query( ctx, addr, &region ) || region.alloc_base != alloc || !region.size) break;
        w->high = region.base + region.size;
        addr = w->high;
    }
    if (w->high <= rsp) return;  /* inconsistent answers: no window */
    w->valid = 1;
    w->budget = ec_window_budget( w->high - w->low );
}

static inline int ec_on_emulator_stack( const struct ec_stack_window *w, uint64_t frame )
{
    if (!w->emu_low && !w->emu_high) return 0;
    return frame >= w->emu_low && frame <= w->emu_high;
}

/* F2 with a window: aligned, and inside the window or the emulator stack (base inclusive) */
static inline int ec_frame_in_window( const struct ec_stack_window *w, uint64_t frame )
{
    if (frame & 7) return 0;
    if (frame >= w->low && frame < w->high) return 1;
    return ec_on_emulator_stack( w, frame );
}

/* F2 without a window, the parts that need no memory query: aligned and in the TEB stack
 * [stack_low, stack_high] or the emulator stack. The PE side adds the committed-private-memory
 * last resort. */
static inline int ec_frame_in_known_stacks( const struct ec_stack_window *w, uint64_t frame,
                                            uint64_t stack_low, uint64_t stack_high )
{
    if (frame & 7) return 0;
    if (frame >= stack_low && frame <= stack_high) return 1;
    return ec_on_emulator_stack( w, frame );
}

/* F3: phase one (call_seh_handlers) */

struct ec_livelock  /* process-wide */
{
    uint64_t pc;
    uint64_t frame;
    uint32_t count;
};

struct ec_seh_walk  /* per walk */
{
    uint32_t steps;          /* completed unwind steps */
    int      terminal_used;
    int      have_prev;
    uint64_t prev_pc;
    uint64_t prev_frame;
};

enum ec_phase1
{
    EC_P1_CONTINUE,       /* frame may be used */
    EC_P1_END_OF_STACK,   /* stop quietly: unhandled */
    EC_P1_ABANDON,        /* stop with EXCEPTION_STACK_INVALID */
    EC_P1_KILL_PROCESS,   /* same abandoned pair 16 times in a row */
    EC_P1_KILL_THREAD,    /* invalid frame on the first step */
    EC_P1_STACK_INVALID,  /* invalid frame after a good step */
    EC_P1_TERMINAL,       /* the thread's start frame, one past the window: accept */
};

static inline void ec_seh_walk_init( struct ec_seh_walk *walk )
{
    memset( walk, 0, sizeof(*walk) );
}

/* after each unwind step: control_pc is the pc the step unwound (dispatch ControlPc), frame its
 * establisher frame */
static inline enum ec_phase1 ec_seh_after_step( struct ec_seh_walk *walk, struct ec_livelock *ll,
                                                uint64_t control_pc, uint64_t frame )
{
    int stuck;

    if (walk->steps && !control_pc) return EC_P1_END_OF_STACK;
    walk->steps++;
    stuck = walk->steps > EC_PHASE1_MAX_STEPS ||
            (walk->have_prev && walk->prev_pc == control_pc && walk->prev_frame == frame);
    walk->have_prev = 1;
    walk->prev_pc = control_pc;
    walk->prev_frame = frame;
    if (!stuck) return EC_P1_CONTINUE;

    if (ll->count && ll->pc == control_pc && ll->frame == frame) ll->count++;
    else
    {
        ll->pc = control_pc;
        ll->frame = frame;
        ll->count = 1;
    }
    return ll->count >= EC_LIVELOCK_LIMIT ? EC_P1_KILL_PROCESS : EC_P1_ABANDON;
}

/* the verdict on a frame that failed the validity test (frame_ok == 0) or passed it */
static inline enum ec_phase1 ec_seh_frame_verdict( struct ec_seh_walk *walk, const struct ec_stack_window *w,
                                                   int frame_ok, uint64_t frame, uint64_t unwound_rsp )
{
    if (frame_ok) return EC_P1_CONTINUE;
    if (w->valid && !walk->terminal_used && frame == w->high && unwound_rsp == w->high)
    {
        walk->terminal_used = 1;
        return EC_P1_TERMINAL;
    }
    return walk->steps <= 1 ? EC_P1_KILL_THREAD : EC_P1_STACK_INVALID;
}

/* F4: phase two (RtlUnwindEx) */

struct ec_unwind_guard
{
    uint32_t steps;
    uint32_t repeats;
    int      have_last;
    int      last_on_emu;
    uint64_t last_frame;
    uint64_t last_pc;
};

enum ec_phase2
{
    EC_P2_OK,
    EC_P2_BUDGET,
    EC_P2_REPEAT,
    EC_P2_BACKWARDS,
};

static inline void ec_unwind_guard_init( struct ec_unwind_guard *g )
{
    memset( g, 0, sizeof(*g) );
}

static inline enum ec_phase2 ec_unwind_guard_step( struct ec_unwind_guard *g, const struct ec_stack_window *w,
                                                   uint64_t frame, uint64_t pc )
{
    int on_emu;

    if (!w->valid) return EC_P2_OK;
    if (++g->steps > w->budget) return EC_P2_BUDGET;
    on_emu = ec_on_emulator_stack( w, frame );
    if (g->have_last)
    {
        if (frame == g->last_frame && pc == g->last_pc)
        {
            if (++g->repeats >= EC_UNWIND_REPEAT_LIMIT) return EC_P2_REPEAT;
        }
        else g->repeats = 0;

        /* a switch between the guest and emulator stacks starts a new baseline */
        if (on_emu == g->last_on_emu && frame < g->last_frame) return EC_P2_BACKWARDS;
    }
    g->have_last = 1;
    g->last_on_emu = on_emu;
    g->last_frame = frame;
    g->last_pc = pc;
    return EC_P2_OK;
}

static inline const char *ec_phase2_reason( enum ec_phase2 r )
{
    switch (r)
    {
    case EC_P2_BUDGET:    return "step budget exceeded";
    case EC_P2_REPEAT:    return "same frame and pc repeated";
    case EC_P2_BACKWARDS: return "frame moved down";
    default:              return "ok";
    }
}

/* F5: coverage of the code bitmap */

#define EC_CODE_PAGE_SIZE       4096
#define EC_CODE_DEFAULT_LIMIT   (1ull << 47)

/* bytes of the bitmap's allocation from the bitmap onwards, times 8 bits times 4 KB; returns the
 * upstream 2^47 and sets *fallback when that doesn't cover both the bitmap and self_addr */
static inline uint64_t ec_bitmap_coverage( uint64_t bitmap, uint64_t self_addr, ec_query_fn query, void *ctx,
                                           int *fallback )
{
    struct ec_region region;
    uint64_t alloc, addr, end, coverage;
    unsigned int i;

    *fallback = 0;
    if (bitmap && query( ctx, bitmap, &region ))
    {
        alloc = region.alloc_base;
        end = alloc;
        for (addr = alloc, i = 0; i < EC_WINDOW_MAX_REGIONS; i++)
        {
            if (!query( ctx, addr, &region ) || region.alloc_base != alloc || !region.size) break;
            end = region.base + region.size;
            addr = end;
        }
        if (end > bitmap && end - bitmap < (1ull << 40))
        {
            coverage = (end - bitmap) * 8 * EC_CODE_PAGE_SIZE;
            if (coverage > bitmap && coverage > self_addr) return coverage;
        }
    }
    *fallback = 1;
    return EC_CODE_DEFAULT_LIMIT;
}

static inline int ec_bitmap_test( const uint64_t *map, uint64_t coverage, uint64_t ptr )
{
    uint64_t page;

    if (!map || ptr >= coverage) return 0;
    page = ptr / EC_CODE_PAGE_SIZE;
    return (map[page / 64] >> (page & 63)) & 1;
}

#endif /* MYIOSDECK_ARM64EC_UNWIND_GUARD_H */
