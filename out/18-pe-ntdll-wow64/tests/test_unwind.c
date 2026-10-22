// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/* Host tests: spec 18 Part F1-F5 (ARM64EC frame checks, code bitmap limit) and G3 (stall guard). */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "../pe/dlls/ntdll/arm64ec_unwind_guard.h"
#include "../pe/dlls/ntdll/arm64_unwind_stall.h"

/* a fake address space: sorted regions */
struct fake_space
{
    struct ec_region regions[16];
    int count;
    int queries;
};

static int fake_query( void *ctx, uint64_t addr, struct ec_region *out )
{
    struct fake_space *space = ctx;
    int i;

    space->queries++;
    for (i = 0; i < space->count; i++)
    {
        if (addr >= space->regions[i].base && addr - space->regions[i].base < space->regions[i].size)
        {
            *out = space->regions[i];
            return 1;
        }
    }
    return 0;
}

static void add_region( struct fake_space *space, uint64_t base, uint64_t alloc, uint64_t size, int committed )
{
    struct ec_region *r = &space->regions[space->count++];
    r->base = base;
    r->alloc_base = alloc;
    r->size = size;
    r->committed = committed;
}

#define STACK_ALLOC 0x100000000ull
#define EMU_LOW     0x200000000ull
#define EMU_HIGH    0x200100000ull

/* a thread stack: reserved part, guard page, committed part; then a foreign allocation */
static void build_stack( struct fake_space *space )
{
    memset( space, 0, sizeof(*space) );
    add_region( space, STACK_ALLOC, STACK_ALLOC, 0xf0000, 0 );
    add_region( space, STACK_ALLOC + 0xf0000, STACK_ALLOC, 0x1000, 1 );
    add_region( space, STACK_ALLOC + 0xf1000, STACK_ALLOC, 0xf000, 1 );
    add_region( space, STACK_ALLOC + 0x100000, STACK_ALLOC + 0x100000, 0x10000, 1 );
}

static void test_window(void)
{
    struct fake_space space;
    struct ec_stack_window w;

    /* budget bounds */
    CHECK_EQ( ec_window_budget( 0 ), 256 );
    CHECK_EQ( ec_window_budget( 16 * 256 ), 256 );
    CHECK_EQ( ec_window_budget( 16 * 1000 ), 1000 );
    CHECK_EQ( ec_window_budget( 16 * 65536 ), 65536 );
    CHECK_EQ( ec_window_budget( 1ull << 40 ), 65536 );

    /* the whole allocation, several regions, stopping at the foreign one */
    build_stack( &space );
    ec_window_init( &w, STACK_ALLOC + 0xf8000, fake_query, &space, EMU_LOW, EMU_HIGH );
    CHECK( w.valid );
    CHECK_EQ( w.low, STACK_ALLOC );
    CHECK_EQ( w.high, STACK_ALLOC + 0x100000 );
    CHECK_EQ( w.budget, 65536 );

    /* frame validity */
    CHECK( ec_frame_in_window( &w, STACK_ALLOC + 0xf8000 ) );
    CHECK( ec_frame_in_window( &w, STACK_ALLOC ) );
    CHECK( !ec_frame_in_window( &w, STACK_ALLOC + 0xf8004 ) );   /* misaligned */
    CHECK( !ec_frame_in_window( &w, STACK_ALLOC + 0x100000 ) );  /* one past the end */
    CHECK( !ec_frame_in_window( &w, STACK_ALLOC - 8 ) );
    CHECK( ec_frame_in_window( &w, EMU_LOW ) );
    CHECK( ec_frame_in_window( &w, EMU_HIGH ) );                 /* emulator base is inclusive */
    CHECK( !ec_frame_in_window( &w, EMU_HIGH + 8 ) );

    /* Rsp not committed: no window */
    ec_window_init( &w, STACK_ALLOC + 0x1000, fake_query, &space, 0, 0 );
    CHECK( !w.valid );
    ec_window_init( &w, 0x900000000ull, fake_query, &space, 0, 0 );
    CHECK( !w.valid );

    /* without a window: TEB stack or emulator stack */
    CHECK( ec_frame_in_known_stacks( &w, 0x5000, 0x4000, 0x8000 ) );
    CHECK( ec_frame_in_known_stacks( &w, 0x8000, 0x4000, 0x8000 ) );
    CHECK( !ec_frame_in_known_stacks( &w, 0x8008, 0x4000, 0x8000 ) );
    CHECK( !ec_frame_in_known_stacks( &w, 0x5004, 0x4000, 0x8000 ) );
    w.emu_low = EMU_LOW; w.emu_high = EMU_HIGH;
    CHECK( ec_frame_in_known_stacks( &w, EMU_LOW + 0x100, 0x4000, 0x8000 ) );
}

static void test_phase2(void)
{
    struct fake_space space;
    struct ec_stack_window w, none;
    struct ec_unwind_guard g;
    uint64_t f = STACK_ALLOC + 0xf8000;
    uint32_t i;

    build_stack( &space );
    ec_window_init( &w, f, fake_query, &space, EMU_LOW, EMU_HIGH );

    /* moving up is fine, the same frame with another pc is fine */
    ec_unwind_guard_init( &g );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1000 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1004 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f + 0x40, 0x2000 ), EC_P2_OK );

    /* backwards */
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f + 0x20, 0x3000 ), EC_P2_BACKWARDS );

    /* four repeats */
    ec_unwind_guard_init( &g );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1000 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1000 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1000 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1000 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1000 ), EC_P2_REPEAT );
    /* a different step resets the run */
    ec_unwind_guard_init( &g );
    for (i = 0; i < 3; i++) CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1000 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1008 ), EC_P2_OK );
    for (i = 0; i < 3; i++) CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x1008 ), EC_P2_OK );

    /* a switch to the emulator stack and back is legal even though the address goes down */
    ec_unwind_guard_init( &g );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, EMU_LOW + 0x800, 0x1000 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, EMU_LOW + 0x900, 0x1100 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f, 0x2000 ), EC_P2_OK );            /* emu -> guest, lower */
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f + 0x10, 0x2100 ), EC_P2_OK );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, EMU_LOW + 0x100, 0x2200 ), EC_P2_OK );  /* guest -> emu */
    CHECK_EQ( ec_unwind_guard_step( &g, &w, EMU_LOW + 0x80, 0x2300 ), EC_P2_BACKWARDS );

    /* budget */
    w.budget = 300;
    ec_unwind_guard_init( &g );
    for (i = 0; i < 300; i++)
        if (ec_unwind_guard_step( &g, &w, f + 16 * i, 0x1000 + i ) != EC_P2_OK) break;
    CHECK_EQ( i, 300 );
    CHECK_EQ( ec_unwind_guard_step( &g, &w, f + 16 * 300, 0x9000 ), EC_P2_BUDGET );

    /* no window: no guard */
    memset( &none, 0, sizeof(none) );
    ec_unwind_guard_init( &g );
    for (i = 0; i < 10; i++) CHECK_EQ( ec_unwind_guard_step( &g, &none, f, 0x1000 ), EC_P2_OK );
    CHECK( ec_phase2_reason( EC_P2_BACKWARDS )[0] );
}

static void test_phase1(void)
{
    struct fake_space space;
    struct ec_stack_window w;
    struct ec_seh_walk walk;
    struct ec_livelock ll;
    uint64_t f = STACK_ALLOC + 0xf8000;
    int i;

    build_stack( &space );
    ec_window_init( &w, f, fake_query, &space, 0, 0 );
    memset( &ll, 0, sizeof(ll) );

    /* end of stack: pc 0 after a step, but not on the first step */
    ec_seh_walk_init( &walk );
    CHECK_EQ( ec_seh_after_step( &walk, &ll, 0, f ), EC_P1_CONTINUE );
    CHECK_EQ( ec_seh_after_step( &walk, &ll, 0x1000, f + 0x10 ), EC_P1_CONTINUE );
    CHECK_EQ( ec_seh_after_step( &walk, &ll, 0, f + 0x20 ), EC_P1_END_OF_STACK );
    CHECK_EQ( ll.count, 0 );

    /* no progress: abandon; the same pair 16 times in a row ends the process */
    for (i = 1; i <= 16; i++)
    {
        ec_seh_walk_init( &walk );
        CHECK_EQ( ec_seh_after_step( &walk, &ll, 0x1000, f ), EC_P1_CONTINUE );
        CHECK_EQ( ec_seh_after_step( &walk, &ll, 0x1000, f ), i < 16 ? EC_P1_ABANDON : EC_P1_KILL_PROCESS );
        CHECK_EQ( ll.count, i );
    }
    /* a different pair resets the counter */
    ec_seh_walk_init( &walk );
    ec_seh_after_step( &walk, &ll, 0x2000, f );
    CHECK_EQ( ec_seh_after_step( &walk, &ll, 0x2000, f ), EC_P1_ABANDON );
    CHECK_EQ( ll.count, 1 );
    ec_seh_walk_init( &walk );
    ec_seh_after_step( &walk, &ll, 0x1000, f );
    CHECK_EQ( ec_seh_after_step( &walk, &ll, 0x1000, f ), EC_P1_ABANDON );
    CHECK_EQ( ll.count, 1 );

    /* more than 65536 steps */
    ec_seh_walk_init( &walk );
    for (i = 0; i < EC_PHASE1_MAX_STEPS; i++)
        if (ec_seh_after_step( &walk, &ll, 0x1000 + 4 * (uint64_t)i, f + 16 * (uint64_t)i ) != EC_P1_CONTINUE) break;
    CHECK_EQ( i, EC_PHASE1_MAX_STEPS );
    CHECK_EQ( ec_seh_after_step( &walk, &ll, 0x900000, f ), EC_P1_ABANDON );

    /* the terminal frame: once per walk, exactly at the top with Rsp at the top */
    ec_seh_walk_init( &walk );
    ec_seh_after_step( &walk, &ll, 0x1000, f );
    ec_seh_after_step( &walk, &ll, 0x1100, f + 0x100 );
    CHECK_EQ( ec_seh_frame_verdict( &walk, &w, 0, w.high, w.high ), EC_P1_TERMINAL );
    CHECK_EQ( ec_seh_frame_verdict( &walk, &w, 0, w.high, w.high ), EC_P1_STACK_INVALID );
    ec_seh_walk_init( &walk );
    ec_seh_after_step( &walk, &ll, 0x1000, f );
    ec_seh_after_step( &walk, &ll, 0x1100, f + 0x100 );
    CHECK_EQ( ec_seh_frame_verdict( &walk, &w, 0, w.high, w.high + 8 ), EC_P1_STACK_INVALID );
    CHECK_EQ( ec_seh_frame_verdict( &walk, &w, 0, w.high + 8, w.high + 8 ), EC_P1_STACK_INVALID );

    /* a bad frame on the first step ends the thread; a good frame continues */
    ec_seh_walk_init( &walk );
    ec_seh_after_step( &walk, &ll, 0x1000, 0x10 );
    CHECK_EQ( ec_seh_frame_verdict( &walk, &w, 0, 0x10, 0x18 ), EC_P1_KILL_THREAD );
    CHECK_EQ( ec_seh_frame_verdict( &walk, &w, 1, f, f + 8 ), EC_P1_CONTINUE );
}

static void test_bitmap_limit(void)
{
    struct fake_space space;
    uint64_t bitmap = 0x300000000ull, cov;
    int fallback;
    uint64_t map[4] = { 0x5, 0, 0, 1ull << 63 };

    /* a bitmap of 3 regions (0x30000 bytes) covers 0x30000 * 8 * 4096 = 0x180000000 bytes... */
    memset( &space, 0, sizeof(space) );
    add_region( &space, bitmap, bitmap, 0x10000, 1 );
    add_region( &space, bitmap + 0x10000, bitmap, 0x10000, 1 );
    add_region( &space, bitmap + 0x20000, bitmap, 0x10000, 0 );
    add_region( &space, bitmap + 0x30000, bitmap + 0x30000, 0x10000, 1 );  /* foreign: ends the walk */
    cov = ec_bitmap_coverage( bitmap, 0x180000000ull, fake_query, &space, &fallback );
    /* ...which is not larger than the bitmap's own address: too small */
    CHECK( fallback );
    CHECK_EQ( cov, 1ull << 47 );

    /* a bigger bitmap covers itself and the caller */
    space.regions[2].size = 0x100000;
    space.regions[3].base = bitmap + 0x120000;
    space.regions[3].alloc_base = bitmap + 0x120000;
    cov = ec_bitmap_coverage( bitmap, 0x180000000ull, fake_query, &space, &fallback );
    CHECK( !fallback );
    CHECK_EQ( cov, 0x120000ull * 8 * 4096 );

    /* too small for the code address of the caller */
    cov = ec_bitmap_coverage( bitmap, 0x120000ull * 8 * 4096, fake_query, &space, &fallback );
    CHECK( fallback );

    /* NULL or unqueryable bitmap */
    cov = ec_bitmap_coverage( 0, 0x1000, fake_query, &space, &fallback );
    CHECK( fallback );
    cov = ec_bitmap_coverage( 0x700000000ull, 0x1000, fake_query, &space, &fallback );
    CHECK( fallback );

    /* the test itself */
    CHECK( ec_bitmap_test( map, 0x100000, 0 ) );
    CHECK( !ec_bitmap_test( map, 0x100000, 0x1000 ) );
    CHECK( ec_bitmap_test( map, 0x100000, 0x2fff ) );
    CHECK( ec_bitmap_test( map, 0x100000, 255ull * 4096 ) );
    CHECK( !ec_bitmap_test( map, 255ull * 4096, 255ull * 4096 ) );  /* at the coverage */
    CHECK( !ec_bitmap_test( NULL, 0x100000, 0 ) );
    CHECK( !ec_bitmap_test( map, 0x100000, ~0ull ) );
}

static void test_stall(void)
{
    struct arm64_stall_guard g;
    int i;

    arm64_stall_init( &g );
    CHECK( !arm64_stall_step( &g, 0x1000, 0x8000 ) );
    CHECK( !arm64_stall_step( &g, 0x1000, 0x8000 ) );  /* 1st unchanged */
    CHECK( !arm64_stall_step( &g, 0x1000, 0x8000 ) );  /* 2nd */
    CHECK( arm64_stall_step( &g, 0x1000, 0x8000 ) );   /* 3rd in a row stops */

    /* progress in between resets the count */
    arm64_stall_init( &g );
    arm64_stall_step( &g, 0x1000, 0x8000 );
    arm64_stall_step( &g, 0x1000, 0x8000 );
    arm64_stall_step( &g, 0x1000, 0x8000 );
    CHECK( !arm64_stall_step( &g, 0x1000, 0x8010 ) );  /* sp moved */
    CHECK( !arm64_stall_step( &g, 0x1000, 0x8010 ) );
    CHECK( !arm64_stall_step( &g, 0x1000, 0x8010 ) );
    CHECK( arm64_stall_step( &g, 0x1000, 0x8010 ) );

    /* pc changing alone is progress */
    arm64_stall_init( &g );
    for (i = 0; i < 100; i++) CHECK( !arm64_stall_step( &g, 0x1000 + 4 * (uint64_t)(i & 1), 0x8000 ) );
}

int main(void)
{
    test_window();
    test_phase2();
    test_phase1();
    test_bitmap_limit();
    test_stall();
    return check_report( "unwind" );
}
