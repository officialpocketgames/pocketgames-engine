// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part M (Part C): winevulkan's call statistics (winevulkan_ios_pure.c) and the entry
// list gen-entries.sh made from Wine 11.4's loader_thunks.h.

#include <stdio.h>
#include <string.h>
#include "winevulkan_ios_entries.h"
#include "winevulkan_ios_pure.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); failures++; } } while (0)

#define NAME(n, name) #name,
static const char *const names[] = { WINEVULKAN_IOS_ENTRIES(NAME) };
#undef NAME
#define INDEX(n, name) n,
static const unsigned int indices[] = { WINEVULKAN_IOS_ENTRIES(INDEX) };
#undef INDEX

static void test_entries( void )
{
    unsigned int i, present = 0;

    CHECK( sizeof(names) / sizeof(names[0]) == WINEVULKAN_IOS_ENTRY_COUNT );
    CHECK( WINEVULKAN_IOS_ENTRY_COUNT > 600 );
    CHECK( !strcmp( names[0], "init" ) );
    CHECK( !strcmp( names[1], "is_available_instance_function" ) );
    for (i = 0; i < WINEVULKAN_IOS_ENTRY_COUNT; i++)
    {
        CHECK( indices[i] == i );
        if (!strcmp( names[i], "vkQueuePresentKHR" )) present++;
    }
    CHECK( present == 1 );
}

static void test_top( void )
{
    uint64_t counts[40] = { 0 };
    unsigned int top[16], n, i;

    CHECK( ivk_top_counts( counts, 40, 16, top ) == 0 );
    counts[3] = 5; counts[7] = 9; counts[8] = 5; counts[30] = 100;
    n = ivk_top_counts( counts, 40, 16, top );
    CHECK( n == 4 && top[0] == 30 && top[1] == 7 && top[2] == 3 && top[3] == 8 );   /* ties: lower index first */
    for (i = 0; i < 40; i++) counts[i] = i + 1;
    n = ivk_top_counts( counts, 40, 16, top );
    CHECK( n == 16 && top[0] == 39 && top[15] == 24 );
    CHECK( ivk_top_counts( counts, 40, 3, top ) == 3 && top[2] == 37 );
}

static void test_line( void )
{
    static const char *const n[] = { "init", "vkCmdDraw", "vkQueuePresentKHR" };
    uint64_t counts[6] = { 0, 120, 3, 0, 40, 0 };
    unsigned int idx[3] = { 1, 4, 2 };
    char line[256];

    ivk_stats_line( line, sizeof(line), idx, 3, counts, n, 3 );
    CHECK( !strcmp( line, "vk calls: vkCmdDraw 120, vkCmdDraw (32-bit) 40, vkQueuePresentKHR 3" ) );
    ivk_stats_line( line, sizeof(line), idx, 0, counts, n, 3 );
    CHECK( !strcmp( line, "vk calls: none" ) );
    ivk_stats_line( line, 24, idx, 3, counts, n, 3 );
    CHECK( !strcmp( line, "vk calls: vkCmdDraw 120" ) );   /* cut at an entry */
    CHECK( ivk_stats_due( 0, 0, 0 ) );
    CHECK( !ivk_stats_due( 19999999999ull, 10000000000ull, 1 ) );
    CHECK( ivk_stats_due( 20000000000ull, 10000000000ull, 1 ) );
}

int main( void )
{
    test_entries();
    test_top();
    test_line();
    if (failures) { printf( "test_winevulkan: %d failure(s)\n", failures ); return 1; }
    printf( "test_winevulkan: all passed (%u entries)\n", (unsigned int)WINEVULKAN_IOS_ENTRY_COUNT );
    return 0;
}
