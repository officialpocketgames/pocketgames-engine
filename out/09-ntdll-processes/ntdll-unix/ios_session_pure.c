// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 22 Part C: pure rules of later programs in a kept session (see ios_session_pure.h).

#include <stddef.h>

#include "ios_session_pure.h"

/* ---------- C6 ---------- */

int ios_hook_list_add( struct ios_hook_list *list, ios_process_end_fn fn )
{
    int i, count, ret = -1;

    if (!fn) return -1;
    /* adders are rare (one per library, at its load): a spin lock among them only */
    while (__atomic_exchange_n( &list->lock, 1, __ATOMIC_ACQUIRE )) ;
    count = __atomic_load_n( &list->count, __ATOMIC_RELAXED );
    for (i = 0; i < count; i++) if (list->fn[i] == fn) ret = 1;
    if (ret != 1 && count < IOS_PROCESS_END_HOOKS_MAX)
    {
        list->fn[count] = fn;
        __atomic_store_n( &list->count, count + 1, __ATOMIC_RELEASE );
        ret = 0;
    }
    __atomic_store_n( &list->lock, 0, __ATOMIC_RELEASE );
    return ret;
}

int ios_hook_list_run( struct ios_hook_list *list, const void *peb )
{
    int i, count = __atomic_load_n( &list->count, __ATOMIC_ACQUIRE );

    for (i = 0; i < count; i++) list->fn[i]( peb );
    return count;
}

/* ---------- C3 ---------- */

int ios_killed_thread_ends_process( int killed, int is_main, int others, int ending )
{
    return killed && is_main && others > 0 && !ending;
}

/* ---------- C5 ---------- */

enum ios_leftover_action ios_leftover_action( int gone, int waiting, int in_wine )
{
    if (gone) return IOS_LEFTOVER_GONE;
    if (!waiting) return IOS_LEFTOVER_RUNNING;
    if (in_wine) return IOS_LEFTOVER_LOCKED;
    return IOS_LEFTOVER_SUSPEND;
}

void ios_leftover_count( struct ios_leftover_counts *c, enum ios_leftover_action a )
{
    switch (a)
    {
    case IOS_LEFTOVER_GONE:    c->left++; break;
    case IOS_LEFTOVER_SUSPEND: c->suspended++; break;
    case IOS_LEFTOVER_LOCKED:
    case IOS_LEFTOVER_RUNNING: c->kept++; break;
    }
}

/* ---------- C8 ---------- */

void *ios_prev_teb_exchange( void **slot, void *self_teb, int keep_self )
{
    return __atomic_exchange_n( slot, keep_self ? self_teb : NULL, __ATOMIC_ACQ_REL );
}
