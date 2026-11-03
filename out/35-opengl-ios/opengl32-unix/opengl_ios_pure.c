// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Parts B1 and E: pure functions of opengl32's iOS wrappers (opengl_ios_pure.h).

#include <stdio.h>
#include <string.h>

#include "opengl_ios_pure.h"

int opengl_ios_switch_on( const char *value )
{
    return value && value[0] == '1' && !value[1];
}

int opengl_ios_report_due( uint64_t now_ns, uint64_t period_start_ns )
{
    return period_start_ns && now_ns >= period_start_ns && now_ns - period_start_ns >= OPENGL_IOS_REPORT_NS;
}

static void append( char *buf, size_t size, size_t *len, const char *fmt, double a, double b, const char *name )
{
    int n;

    if (*len + 1 >= size) return;
    if (name) n = snprintf( buf + *len, size - *len, fmt, name, a, b );
    else n = snprintf( buf + *len, size - *len, fmt, a, b );
    if (n < 0) return;
    *len += (size_t)n < size - *len ? (size_t)n : size - *len - 1;
}

size_t opengl_ios_format_report( char *buf, size_t size, const uint32_t *counts, unsigned int n,
                                 const char *const *names, unsigned int swap_index, uint64_t gate,
                                 double seconds )
{
    unsigned int top[OPENGL_IOS_REPORT_TOP], ntop = 0, i, j;
    double total = 0, swaps;
    size_t len = 0;
    int written;

    if (!size) return 0;
    buf[0] = 0;
    if (seconds <= 0) seconds = 1;
    for (i = 0; i < n; i++) total += counts[i];
    swaps = swap_index < n ? counts[swap_index] : 0;

    written = snprintf( buf, size, "[opengl] calls: %.0f/s, %.1f/swap (%.1f/s), via gate %.0f/s; top:",
                        total / seconds, swaps ? total / swaps : 0.0, swaps / seconds, (double)gate / seconds );
    if (written < 0) return 0;
    len = (size_t)written < size ? (size_t)written : size - 1;

    /* the busiest entries, by insertion into a short sorted list */
    for (i = 0; i < n; i++)
    {
        if (!counts[i]) continue;
        for (j = ntop; j > 0 && counts[top[j - 1]] < counts[i]; j--)
            if (j < OPENGL_IOS_REPORT_TOP) top[j] = top[j - 1];
        if (j < OPENGL_IOS_REPORT_TOP)
        {
            top[j] = i;
            if (ntop < OPENGL_IOS_REPORT_TOP) ntop++;
        }
    }
    for (i = 0; i < ntop; i++)
        append( buf, size, &len, i ? ", %s %.0f/s (%.1f/swap)" : " %s %.0f/s (%.1f/swap)",
                counts[top[i]] / seconds, swaps ? counts[top[i]] / swaps : 0.0,
                names && names[top[i]] ? names[top[i]] : "?" );
    return len;
}
