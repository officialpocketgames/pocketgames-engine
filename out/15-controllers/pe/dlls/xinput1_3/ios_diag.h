// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 9: the diagnostic counters every reader path uses. Each path logs its
// first few events in full, then one count per 5 s window while events keep coming. Plain C,
// header only, no Wine headers. This copy (hidpad/) is the one the server and the host test
// tests/test_diag.c use; the Windows-side DLLs carry identical copies in their own folders
// (pe/dlls/dinput, pe/dlls/xinput1_3, pe/dlls/windows.gaming.input), because Wine's makedep
// finds a header of include/ only when include/Makefile.in lists it. run-host-tests.sh fails
// when a copy differs. Diagnostics only: nothing here changes what a reader gets.

#ifndef MYIOSDECK_IOS_DIAG_H
#define MYIOSDECK_IOS_DIAG_H

#include <stdint.h>

#define IOS_DIAG_WINDOW_MS 5000u

struct ios_diag
{
    uint32_t total;             /* events since the start */
    uint32_t window;            /* events in the current window */
    uint64_t window_start_ms;   /* when the current window began */
};

/* Count one event at `now_ms`; returns its number (1-based), so the caller logs it in full while
 * the number is at most its "first few". The first event starts the first window. */
static inline uint32_t ios_diag_event( struct ios_diag *d, uint64_t now_ms )
{
    if (!d->total) d->window_start_ms = now_ms;
    d->total++;
    d->window++;
    return d->total;
}

/* The count of the window when it has lasted IOS_DIAG_WINDOW_MS and had events (the next
 * window then starts at `now_ms`), else 0. `*elapsed_ms` (may be NULL) gets the window's length,
 * for a per-second rate. */
static inline uint32_t ios_diag_due( struct ios_diag *d, uint64_t now_ms, uint64_t *elapsed_ms )
{
    uint32_t n;

    if (!d->window || now_ms < d->window_start_ms || now_ms - d->window_start_ms < IOS_DIAG_WINDOW_MS)
        return 0;
    n = d->window;
    if (elapsed_ms) *elapsed_ms = now_ms - d->window_start_ms;
    d->window = 0;
    d->window_start_ms = now_ms;
    return n;
}

/* A window for a summary that is logged even without events (the server's "handle open but
 * never read" case): due every IOS_DIAG_WINDOW_MS from `*start_ms`, which it advances. */
static inline int ios_diag_period( uint64_t *start_ms, uint64_t now_ms )
{
    if (!*start_ms || now_ms < *start_ms)
    {
        *start_ms = now_ms;
        return 0;
    }
    if (now_ms - *start_ms < IOS_DIAG_WINDOW_MS) return 0;
    *start_ms = now_ms;
    return 1;
}

#endif
