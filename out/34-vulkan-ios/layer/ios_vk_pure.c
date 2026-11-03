// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E: see ios_vk_pure.h. No Vulkan, no Apple API.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ios_vk_pure.h"

/* ---------------------------------------------------------------- E4 */

uint64_t ivk_mem_limit( uint64_t footprint, uint64_t available, uint64_t memsize )
{
    const uint64_t reserve = 1536 * IVK_MB;   /* spec 00 Part I: ram-reserve-mb is 1536 */
    uint64_t limit = UINT64_MAX, cap = UINT64_MAX;

    if (available)
        limit = footprint > UINT64_MAX - available ? UINT64_MAX : footprint + available;
    if (memsize) cap = memsize > reserve ? memsize - reserve : 0;
    return limit < cap ? limit : cap;
}

uint64_t ivk_heap_clamp( uint64_t limit, uint64_t driver_size )
{
    const uint64_t taken = (4096 + 2560) * IVK_MB, floor = 1024 * IVK_MB;
    uint64_t size = limit > taken ? limit - taken : 0;

    if (limit == UINT64_MAX) size = UINT64_MAX;   /* nothing known: the driver's size */
    if (size < floor) size = floor;
    if (size > driver_size) size = driver_size;
    return size;
}

uint64_t ivk_budget_trim( uint64_t clamped, uint64_t limit, uint64_t footprint )
{
    const uint64_t trim = 1536 * IVK_MB, floor = 768 * IVK_MB;
    uint64_t budget = clamped;

    if (limit != UINT64_MAX && limit > trim && footprint > limit - trim)
    {
        uint64_t excess = footprint - (limit - trim);
        budget = excess < clamped ? clamped - excess : 0;
    }
    if (budget < floor) budget = floor;
    if (budget > clamped) budget = clamped;
    return budget;
}

int ivk_budget_due( uint64_t now_ns, uint64_t last_ns, int have_last )
{
    return !have_last || now_ns < last_ns || now_ns - last_ns >= IVK_BUDGET_REREAD_NS;
}

/* ---------------------------------------------------------------- E5 */

uint64_t ivk_pace_period_ns( int mode )
{
    switch (mode)
    {
    case 1: return 1000000000ull / 60;
    case 3: return 1000000000ull / 30;
    case 4: return 1000000000ull / 40;
    default: return 0;   /* 0: the display paces; 2: uncapped */
    }
}

uint64_t ivk_pace( struct ivk_pacer *pacer, int mode, uint64_t now_ns )
{
    uint64_t period = ivk_pace_period_ns( mode ), next;

    if (!period)
    {
        pacer->primed = 0;
        pacer->mode = mode;
        return 0;
    }
    if (!pacer->primed || pacer->mode != mode)
    {
        pacer->primed = 1;
        pacer->mode = mode;
        pacer->deadline = now_ns;
        return 0;
    }
    next = pacer->deadline + period;
    if (now_ns > next && now_ns - next > period) next = now_ns;   /* more than a period late */
    pacer->deadline = next;
    return next > now_ns ? next : 0;
}

/* ---------------------------------------------------------------- E7 */

int32_t ivk_sliced_wait( uint64_t timeout_ns, ivk_wait_call call, void *ctx,
                         const struct ivk_wait_env *env, int not_ready_is_timeout )
{
    int forever = timeout_ns == UINT64_MAX;
    uint64_t start, deadline = 0;

    if (timeout_ns <= IVK_WAIT_SLICE_NS) return call( ctx, timeout_ns );

    start = env->now_ns();
    if (!forever) deadline = start > UINT64_MAX - timeout_ns ? UINT64_MAX - 1 : start + timeout_ns;
    for (;;)
    {
        uint64_t now = env->now_ns(), remaining, slice;
        int32_t res;

        remaining = forever ? UINT64_MAX : (deadline > now ? deadline - now : 0);
        slice = remaining < IVK_WAIT_SLICE_NS ? remaining : IVK_WAIT_SLICE_NS;
        res = call( ctx, slice );
        if (res == IVK_NOT_READY && not_ready_is_timeout) res = IVK_TIMEOUT;
        if (res != IVK_TIMEOUT) return res;
        if (!forever && env->now_ns() >= deadline) return IVK_TIMEOUT;
        if (env->kill_pending && env->kill_pending() == 1) return IVK_ERROR_DEVICE_LOST;
    }
}

/* ---------------------------------------------------------------- E1 */

#define IVK_TOMB UINT64_MAX

static unsigned int map_hash( uint64_t key )
{
    key ^= key >> 33;
    key *= 0x9e3779b97f4a7c15ull;
    return (unsigned int)(key >> 32) & (IVK_MAP_CAP - 1);
}

void *ivk_map_get( struct ivk_map *map, uint64_t key )
{
    unsigned int i = map_hash( key ), n;

    for (n = 0; n < IVK_MAP_CAP; n++, i = (i + 1) & (IVK_MAP_CAP - 1))
    {
        uint64_t k = atomic_load_explicit( &map->keys[i], memory_order_acquire );
        if (k == key) return atomic_load_explicit( &map->vals[i], memory_order_acquire );
        if (!k) return NULL;
    }
    return NULL;
}

int ivk_map_put( struct ivk_map *map, uint64_t key, void *val )
{
    unsigned int i = map_hash( key ), n, free_slot = IVK_MAP_CAP;

    if (!key || key == IVK_TOMB) return 0;
    for (n = 0; n < IVK_MAP_CAP; n++, i = (i + 1) & (IVK_MAP_CAP - 1))
    {
        uint64_t k = atomic_load_explicit( &map->keys[i], memory_order_relaxed );
        if (k == key)
        {
            atomic_store_explicit( &map->vals[i], val, memory_order_release );
            return 1;
        }
        if (k == IVK_TOMB && free_slot == IVK_MAP_CAP) free_slot = i;
        if (!k)
        {
            if (free_slot == IVK_MAP_CAP) free_slot = i;
            break;
        }
    }
    if (free_slot == IVK_MAP_CAP) return 0;
    /* the record first, then the key that makes it visible */
    atomic_store_explicit( &map->vals[free_slot], val, memory_order_release );
    atomic_store_explicit( &map->keys[free_slot], key, memory_order_release );
    map->count++;
    return 1;
}

void *ivk_map_del( struct ivk_map *map, uint64_t key )
{
    unsigned int i = map_hash( key ), n;

    for (n = 0; n < IVK_MAP_CAP; n++, i = (i + 1) & (IVK_MAP_CAP - 1))
    {
        uint64_t k = atomic_load_explicit( &map->keys[i], memory_order_relaxed );
        if (k == key)
        {
            void *old = atomic_load_explicit( &map->vals[i], memory_order_relaxed );
            atomic_store_explicit( &map->vals[i], NULL, memory_order_release );
            atomic_store_explicit( &map->keys[i], IVK_TOMB, memory_order_release );
            map->count--;
            return old;
        }
        if (!k) return NULL;
    }
    return NULL;
}

void *ivk_map_slot( struct ivk_map *map, unsigned int i, uint64_t *key )
{
    uint64_t k = atomic_load_explicit( &map->keys[i], memory_order_acquire );

    if (!k || k == IVK_TOMB) return NULL;
    if (key) *key = k;
    return atomic_load_explicit( &map->vals[i], memory_order_acquire );
}

/* ---------------------------------------------------------------- E8 */

static const char *const kind_names[IVK_KIND_COUNT] =
{
    "swapchains", "framebuffers", "render passes", "pipelines", "pipeline caches",
    "shader modules", "pipeline layouts", "descriptor pools", "descriptor set layouts",
    "command pools", "query pools", "events", "fences", "semaphores", "buffer views",
    "image views", "samplers", "images", "buffers", "memory",
};

const char *ivk_kind_name( enum ivk_kind kind )
{
    return (unsigned int)kind < IVK_KIND_COUNT ? kind_names[kind] : "?";
}

static uint32_t set_hash( uint64_t handle, uint32_t cap )
{
    handle ^= handle >> 29;
    handle *= 0xbf58476d1ce4e5b9ull;
    return (uint32_t)(handle >> 32) & (cap - 1);
}

static int set_grow( struct ivk_set *set )
{
    uint32_t cap = set->cap ? set->cap * 2 : 64, i;
    uint64_t *slots, *old = set->slots;

    /* only rehash in place when deleted slots, not live ones, fill it */
    if (set->cap && set->count * 2 < set->cap) cap = set->cap;
    if (!(slots = calloc( cap, sizeof(*slots) ))) return 0;
    for (i = 0; i < set->cap; i++)
    {
        uint64_t h = old[i];
        uint32_t j;
        if (!h || h == IVK_TOMB) continue;
        for (j = set_hash( h, cap ); slots[j]; j = (j + 1) & (cap - 1)) {}
        slots[j] = h;
    }
    free( old );
    set->slots = slots;
    set->cap = cap;
    set->used = set->count;
    return 1;
}

int ivk_set_add( struct ivk_set *set, uint64_t handle )
{
    uint32_t i, tomb = UINT32_MAX;

    if (!handle || handle == IVK_TOMB) return 0;
    if ((set->used + 1) * 4 > set->cap * 3 && !set_grow( set )) return 0;
    for (i = set_hash( handle, set->cap ); set->slots[i]; i = (i + 1) & (set->cap - 1))
    {
        if (set->slots[i] == handle) return 1;
        if (set->slots[i] == IVK_TOMB && tomb == UINT32_MAX) tomb = i;
    }
    if (tomb != UINT32_MAX) i = tomb;
    else set->used++;
    set->slots[i] = handle;
    set->count++;
    return 1;
}

static uint32_t set_find( const struct ivk_set *set, uint64_t handle )
{
    uint32_t i;

    if (!set->cap || !handle || handle == IVK_TOMB) return UINT32_MAX;
    for (i = set_hash( handle, set->cap ); set->slots[i]; i = (i + 1) & (set->cap - 1))
        if (set->slots[i] == handle) return i;
    return UINT32_MAX;
}

int ivk_set_del( struct ivk_set *set, uint64_t handle )
{
    uint32_t i = set_find( set, handle );

    if (i == UINT32_MAX) return 0;
    set->slots[i] = IVK_TOMB;
    set->count--;
    return 1;
}

int ivk_set_has( const struct ivk_set *set, uint64_t handle )
{
    return set_find( set, handle ) != UINT32_MAX;
}

void ivk_set_each( const struct ivk_set *set, void (*fn)( void *ctx, uint64_t handle ), void *ctx )
{
    uint32_t i;

    for (i = 0; i < set->cap; i++)
        if (set->slots[i] && set->slots[i] != IVK_TOMB) fn( ctx, set->slots[i] );
}

void ivk_set_free( struct ivk_set *set )
{
    free( set->slots );
    memset( set, 0, sizeof(*set) );
}

struct release_ctx
{
    void (*destroy)( void *ctx, enum ivk_kind kind, uint64_t handle );
    void *ctx;
    enum ivk_kind kind;
    unsigned int count;
};

static void release_one( void *p, uint64_t handle )
{
    struct release_ctx *rc = p;
    rc->destroy( rc->ctx, rc->kind, handle );
    rc->count++;
}

unsigned int ivk_release_all( struct ivk_set sets[IVK_KIND_COUNT],
                              void (*destroy)( void *ctx, enum ivk_kind kind, uint64_t handle ),
                              void *ctx, unsigned int counts[IVK_KIND_COUNT] )
{
    unsigned int total = 0;
    int kind;

    for (kind = 0; kind < IVK_KIND_COUNT; kind++)
    {
        struct release_ctx rc = { destroy, ctx, (enum ivk_kind)kind, 0 };
        ivk_set_each( &sets[kind], release_one, &rc );
        ivk_set_free( &sets[kind] );
        if (counts) counts[kind] = rc.count;
        total += rc.count;
    }
    return total;
}

void ivk_release_summary( const unsigned int counts[IVK_KIND_COUNT], char *buf, size_t size )
{
    size_t len = 0;
    int kind, any = 0;

    if (!size) return;
    buf[0] = 0;
    for (kind = 0; kind < IVK_KIND_COUNT; kind++)
    {
        int n;
        if (!counts[kind]) continue;
        n = snprintf( buf + len, size - len, "%s%u %s", any ? ", " : "", counts[kind], kind_names[kind] );
        if (n < 0 || (size_t)n >= size - len)
        {
            buf[len] = 0;   /* what doesn't fit is left out */
            break;
        }
        len += (size_t)n;
        any = 1;
    }
    if (!any) snprintf( buf, size, "no objects" );
}

/* ---------------------------------------------------------------- logs */

int ivk_pid_first( struct ivk_pidset *set, unsigned int pid )
{
    unsigned int i, n = sizeof(set->pids) / sizeof(set->pids[0]);

    for (i = 0; i < set->used; i++) if (set->pids[i] == pid) return 0;
    set->pids[set->next] = pid;
    set->next = (set->next + 1) % n;
    if (set->used < n) set->used++;
    return 1;
}

void ivk_names_init( struct ivk_names *names )
{
    memset( names, 0, sizeof(*names) );
}

void ivk_names_add( struct ivk_names *names, const char *name )
{
    size_t room = sizeof(names->buf) - names->len, need = strlen( name ) + (names->count ? 2 : 0);

    names->count++;
    if (names->cut) return;
    if (need + 4 > room)   /* keep space for "..." and the NUL */
    {
        memcpy( names->buf + names->len, "...", 4 );
        names->len += 3;
        names->cut = 1;
        return;
    }
    if (names->count > 1)
    {
        memcpy( names->buf + names->len, ", ", 2 );
        names->len += 2;
    }
    memcpy( names->buf + names->len, name, strlen( name ) + 1 );
    names->len += strlen( name );
}

unsigned int ivk_bool_diff( const uint32_t *want, const uint32_t *have, unsigned int count,
                            const char *fields, struct ivk_names *out )
{
    unsigned int i, missing = 0;
    const char *name = fields;

    for (i = 0; i < count; i++, name += strlen( name ) + 1)
    {
        if (!want[i] || have[i]) continue;
        ivk_names_add( out, *name ? name : "?" );
        missing++;
    }
    return missing;
}
