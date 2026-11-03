/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 34 Part C5: see winevulkan_ios_pure.h.
 */

#include <stdio.h>
#include <string.h>
#include "winevulkan_ios_pure.h"

int ivk_stats_due( uint64_t now_ns, uint64_t last_ns, int have_last )
{
    return !have_last || now_ns < last_ns || now_ns - last_ns >= IVK_STATS_PERIOD_NS;
}

unsigned int ivk_top_counts( const uint64_t *counts, unsigned int n, unsigned int k, unsigned int *out )
{
    unsigned int found = 0, i, j;

    for (i = 0; i < n; i++)
    {
        if (!counts[i]) continue;
        /* insertion into the sorted list of the best so far */
        for (j = found; j > 0 && counts[out[j - 1]] < counts[i]; j--)
            if (j < k) out[j] = out[j - 1];
        if (j < k)
        {
            out[j] = i;
            if (found < k) found++;
        }
    }
    return found;
}

void ivk_stats_line( char *buf, size_t size, const unsigned int *idx, unsigned int n, const uint64_t *counts,
                     const char *const *names, unsigned int per_table )
{
    size_t len;
    unsigned int i;

    if (!size) return;
    len = (size_t)snprintf( buf, size, "vk calls:" );
    for (i = 0; i < n && len < size; i++)
    {
        unsigned int e = idx[i] >= per_table ? idx[i] - per_table : idx[i];
        int w = snprintf( buf + len, size - len, "%s %s%s %llu", i ? "," : "", names[e],
                          idx[i] >= per_table ? " (32-bit)" : "", (unsigned long long)counts[idx[i]] );
        if (w < 0 || (size_t)w >= size - len)
        {
            buf[len] = 0;   /* what doesn't fit is left out */
            return;
        }
        len += (size_t)w;
    }
    if (!n && len < size) snprintf( buf + len, size - len, " none" );
}
