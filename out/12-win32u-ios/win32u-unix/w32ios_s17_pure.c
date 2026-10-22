// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Parts F and G: the pure rules (see w32ios_s17_pure.h).

#include "w32ios_s17_pure.h"

int w32ios_s17_session_32bit( const char *switch_value, int armed )
{
    if (switch_value && switch_value[0]) return !(switch_value[0] == '0' && !switch_value[1]);
    return armed > 0;
}

enum w32ios_s17_publish w32ios_s17_publish_kind( int process_is_32bit, int session_32bit )
{
    return (process_is_32bit && session_32bit) ? W32IOS_S17_VIEW : W32IOS_S17_MASTER;
}

int w32ios_s17_views_find( const struct w32ios_s17_views *views, uint32_t pid, const void *peb )
{
    int i;

    for (i = 0; i < W32IOS_S17_VIEWS; i++)
        if (views->slot[i].stamp && views->slot[i].pid == pid && views->slot[i].peb == peb) return i;
    return -1;
}

int w32ios_s17_views_put( struct w32ios_s17_views *views, uint32_t pid, const void *peb, void *view )
{
    int i, pick = w32ios_s17_views_find( views, pid, peb );

    if (pick < 0)
    {
        /* an empty slot, else the oldest (a dead process's view died with its window) */
        pick = 0;
        for (i = 0; i < W32IOS_S17_VIEWS; i++)
        {
            if (!views->slot[i].stamp) { pick = i; break; }
            if (views->slot[i].stamp < views->slot[pick].stamp) pick = i;
        }
    }
    views->slot[pick].pid = pid;
    views->slot[pick].peb = peb;
    views->slot[pick].view = view;
    views->slot[pick].stamp = ++views->clock;
    return pick;
}

int w32ios_s17_owner_ok( const struct w32ios_s17_owner *caller, const struct w32ios_s17_owner *owner )
{
    if (!caller->is_32bit && !owner->is_32bit) return 1;
    return caller->pid == owner->pid && caller->peb == owner->peb;
}

uint64_t w32ios_s17_root_walk( uint64_t hwnd, w32ios_s17_parent_fn get_parent,
                               w32ios_s17_is_child_fn is_child, void *ctx, unsigned int max_steps )
{
    uint64_t current = hwnd;
    unsigned int steps;

    for (steps = 0; steps < max_steps && is_child( ctx, current ); steps++)
    {
        uint64_t parent = get_parent( ctx, current );
        if (!parent) break;
        current = parent;
    }
    return current;
}
