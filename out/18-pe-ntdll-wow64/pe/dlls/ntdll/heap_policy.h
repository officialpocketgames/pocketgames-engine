// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part I: the 32-bit heap policies, the pure parts.
 *   I1  a switch is on only when its value is exactly "1";
 *   I2  grow sizes: COMPACT caps the grow size at 2 MB and retries a failed sub-heap with half
 *       the grow size, never below the need rounded up to the region alignment, failing once the
 *       grow size is no larger than that; without COMPACT the retry rule is upstream's.
 * Pure code shared by heap.c and the host tests.
 */

#ifndef MYIOSDECK_HEAP_POLICY_H
#define MYIOSDECK_HEAP_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_COMPACT_MAX_GROW      0x200000
#define HEAP_UPSTREAM_RETRY_FLOOR  (4 * 1024 * 1024)

/* value: the variable's UTF-16 value, len characters, no terminator needed */
static inline int heap_policy_value_on( const unsigned short *value, size_t len )
{
    return value && len == 1 && value[0] == '1';
}

static inline size_t heap_policy_round_up( size_t size, size_t align )
{
    return (size + align - 1) & ~(align - 1);
}

/* grow size after a sub-heap was created */
static inline size_t heap_policy_grow_after_success( size_t grow, size_t upstream_max, int compact )
{
    size_t max = compact ? HEAP_COMPACT_MAX_GROW : upstream_max;
    size_t next = grow > max / 2 ? max : grow * 2;
    return next < max ? next : max;
}

/* first grow size to use for a heap (upstream's, capped under COMPACT) */
static inline size_t heap_policy_initial_grow( size_t grow, int compact )
{
    return compact && grow > HEAP_COMPACT_MAX_GROW ? HEAP_COMPACT_MAX_GROW : grow;
}

/* the grow size to retry with after creating a sub-heap failed, or 0 to fail;
 * the sub-heap tried is max(grow, need) as upstream */
static inline size_t heap_policy_retry_grow( size_t grow, size_t need, size_t align, int compact )
{
    size_t floor, next;

    if (!compact)
    {
        if (grow <= need || grow <= HEAP_UPSTREAM_RETRY_FLOOR) return 0;
        return grow / 2;
    }
    floor = heap_policy_round_up( need, align );
    if (floor < need || grow <= floor) return 0;
    next = grow / 2;
    return next < floor ? floor : next;
}

/* I6: the census of large blocks by allocation site, without allocating: the first
 * HEAP_CENSUS_SITES distinct sites are tracked, the rest summed as untracked */

#define HEAP_CENSUS_SITES 32
#define HEAP_CENSUS_TOP   6

struct heap_census_site
{
    uintptr_t site;
    uint64_t  bytes;
    uint32_t  blocks;
};

struct heap_census
{
    struct heap_census_site sites[HEAP_CENSUS_SITES];
    uint32_t                count;
    uint64_t                untracked;  /* bytes of sites past the first 32 */
};

static inline void heap_census_add( struct heap_census *census, uintptr_t site, uint64_t bytes )
{
    uint32_t i;

    for (i = 0; i < census->count; i++)
    {
        if (census->sites[i].site != site) continue;
        census->sites[i].bytes += bytes;
        census->sites[i].blocks++;
        return;
    }
    if (census->count == HEAP_CENSUS_SITES)
    {
        census->untracked += bytes;
        return;
    }
    census->sites[census->count].site = site;
    census->sites[census->count].bytes = bytes;
    census->sites[census->count].blocks = 1;
    census->count++;
}

/* indexes of the (at most) 6 sites holding the most bytes, largest first; returns how many,
 * and adds the bytes of the other tracked sites to *rest */
static inline uint32_t heap_census_top( const struct heap_census *census, uint32_t top[HEAP_CENSUS_TOP],
                                        uint64_t *rest )
{
    uint32_t i, j, n = 0;
    int used[HEAP_CENSUS_SITES] = { 0 };

    for (j = 0; j < HEAP_CENSUS_TOP && j < census->count; j++)
    {
        uint32_t best = HEAP_CENSUS_SITES;
        for (i = 0; i < census->count; i++)
            if (!used[i] && (best == HEAP_CENSUS_SITES || census->sites[i].bytes > census->sites[best].bytes))
                best = i;
        used[best] = 1;
        top[n++] = best;
    }
    for (i = 0; i < census->count; i++) if (!used[i]) *rest += census->sites[i].bytes;
    return n;
}

#endif /* MYIOSDECK_HEAP_POLICY_H */
