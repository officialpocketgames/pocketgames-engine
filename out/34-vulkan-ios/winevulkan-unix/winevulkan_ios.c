/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 34 Part C2 and C5: winevulkan's unix-call tables as the app exports them.
 *
 * C2: spec 09's loader looks up winevulkan_unix_call_funcs and winevulkan_unix_call_wow64_funcs
 * for winevulkan.dll (default visibility, kept by the linker: spec 09 clarification 6). Upstream's
 * generated tables stay in vulkan_thunks.c under internal names (winevulkan_ios_config.h).
 * C5: each entry here counts its calls when PG_VK_CALL_STATS=1 (read once, at the first init
 * call), then goes on to upstream's entry of the same index. With the variable unset that is one
 * load and one branch per call. Every 10 s, at a present, one line "vk calls:" names the 16
 * largest counts since the last line.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "vulkan_loader.h"
#include "winevulkan_ios_entries.h"
#include "winevulkan_ios_pure.h"

C_ASSERT( WINEVULKAN_IOS_ENTRY_COUNT == unix_count );

static int stats_on;
static pthread_once_t stats_once = PTHREAD_ONCE_INIT;
static _Atomic uint64_t counts[2 * unix_count];
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t stats_last;
static int stats_have_last;

#define NAME(n, name) #name,
static const char *const entry_names[] = { WINEVULKAN_IOS_ENTRIES(NAME) };
#undef NAME

static void stats_read_env( void )
{
    const char *env = getenv( "PG_VK_CALL_STATS" );
    int on = env && !strcmp( env, "1" );

    __atomic_store_n( &stats_on, on, __ATOMIC_RELAXED );
    if (on) __wine_dbg_output( "[vk-ios] Vulkan call statistics on (PG_VK_CALL_STATS=1)\n" );
}

static uint64_t stats_now( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void stats_log( void )
{
    static uint64_t snapshot[2 * unix_count];
    unsigned int top[16], n, i;
    char line[1024];
    uint64_t now = stats_now();

    if (pthread_mutex_trylock( &stats_lock )) return;   /* another present is writing it */
    if (ivk_stats_due( now, stats_last, stats_have_last ))
    {
        for (i = 0; i < 2 * unix_count; i++) snapshot[i] = atomic_exchange_explicit( &counts[i], 0, memory_order_relaxed );
        if (stats_have_last)   /* the first present starts the first period */
        {
            n = ivk_top_counts( snapshot, 2 * unix_count, 16, top );
            ivk_stats_line( line, sizeof(line) - 1, top, n, snapshot, entry_names, unix_count );
            strcat( line, "\n" );
            __wine_dbg_output( "[vk-ios] " );
            __wine_dbg_output( line );
        }
        stats_last = now;
        stats_have_last = 1;
    }
    pthread_mutex_unlock( &stats_lock );
}

static void stats_hit( unsigned int index )
{
    atomic_fetch_add_explicit( &counts[index], 1, memory_order_relaxed );
    if (index == unix_vkQueuePresentKHR || index == unix_count + unix_vkQueuePresentKHR) stats_log();
}

/* entry n of table t (0: 64-bit, 1: WoW64); the init entry (0) reads the switch first */
#define STAT_CHECK(t, n) \
    do { \
        if ((n) == 0) pthread_once( &stats_once, stats_read_env ); \
        if (__builtin_expect( __atomic_load_n( &stats_on, __ATOMIC_RELAXED ), 0 )) stats_hit( (t) * unix_count + (n) ); \
    } while (0)

#define STAT64(n, name) \
    static NTSTATUS stat64_##name( void *args ) { STAT_CHECK( 0, n ); return winevulkan_upstream_funcs[n]( args ); }
#define STAT32(n, name) \
    static NTSTATUS stat32_##name( void *args ) { STAT_CHECK( 1, n ); return winevulkan_upstream_wow64_funcs[n]( args ); }
WINEVULKAN_IOS_ENTRIES(STAT64)
WINEVULKAN_IOS_ENTRIES(STAT32)
#undef STAT64
#undef STAT32

#define ENTRY64(n, name) stat64_##name,
#define ENTRY32(n, name) stat32_##name,

__attribute__((visibility("default"), used))
const unixlib_entry_t winevulkan_unix_call_funcs[] = { WINEVULKAN_IOS_ENTRIES(ENTRY64) };

__attribute__((visibility("default"), used))
const unixlib_entry_t winevulkan_unix_call_wow64_funcs[] = { WINEVULKAN_IOS_ENTRIES(ENTRY32) };

C_ASSERT( ARRAY_SIZE(winevulkan_unix_call_funcs) == unix_count );
C_ASSERT( ARRAY_SIZE(winevulkan_unix_call_wow64_funcs) == unix_count );
C_ASSERT( ARRAY_SIZE(entry_names) == unix_count );
