/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 34 Part C5: the call statistics' arithmetic, without Wine or Vulkan headers
 * (tests/test_winevulkan.c drives it).
 */

#ifndef WINEVULKAN_IOS_PURE_H
#define WINEVULKAN_IOS_PURE_H

#include <stddef.h>
#include <stdint.h>

/* the line is written at most every 10 s, at a present */
#define IVK_STATS_PERIOD_NS (10ull * 1000 * 1000 * 1000)
int ivk_stats_due( uint64_t now_ns, uint64_t last_ns, int have_last );

/* the k largest non-zero counts, largest first (equal counts: lower index first); their indices go
 * to out; returns how many */
unsigned int ivk_top_counts( const uint64_t *counts, unsigned int n, unsigned int k, unsigned int *out );

/* "vk calls: <name> <count>, ..." for the given indices into counts. Indices of the second table
 * (index >= per_table) name entry index - per_table with " (32-bit)" after it. */
void ivk_stats_line( char *buf, size_t size, const unsigned int *idx, unsigned int n, const uint64_t *counts,
                     const char *const *names, unsigned int per_table );

#endif /* WINEVULKAN_IOS_PURE_H */
