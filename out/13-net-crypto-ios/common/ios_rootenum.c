// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part F2: per-thread positions in the shared root certificate list. See
// include/ios_rootenum.h for the rules.

#include <string.h>

#include "ios_rootenum.h"

int ios_rootenum_slot_of( const struct ios_rootenum *e, uint64_t thread )
{
    int i;

    for (i = 0; i < IOS_ROOTENUM_SLOTS; i++)
        if (e->pos[i].in_use && e->pos[i].thread == thread) return i;
    return -1;
}

/* the thread's position, starting over after IOS_ROOTENUM_IDLE_MS; a new thread takes a free
 * slot, else the one used longest ago */
static struct ios_rootenum_pos *get_pos( struct ios_rootenum *e, uint64_t thread, uint64_t now_ms )
{
    struct ios_rootenum_pos *p, *oldest = NULL;
    int i = ios_rootenum_slot_of( e, thread );

    if (i >= 0)
    {
        p = &e->pos[i];
        if (now_ms - p->last_used_ms >= IOS_ROOTENUM_IDLE_MS) p->index = 0;
        return p;
    }

    for (i = 0; i < IOS_ROOTENUM_SLOTS; i++)
    {
        p = &e->pos[i];
        if (!p->in_use) { oldest = p; break; }
        if (!oldest || p->last_used_ms < oldest->last_used_ms) oldest = p;
    }
    oldest->in_use = 1;
    oldest->thread = thread;
    oldest->index = 0;
    oldest->last_used_ms = now_ms;
    return oldest;
}

enum ios_rootenum_result ios_rootenum_next( struct ios_rootenum *e, uint64_t thread,
                                            uint64_t now_ms, size_t count,
                                            const size_t *sizes,
                                            const unsigned char *const *data,
                                            void *buffer, size_t size, size_t *needed )
{
    struct ios_rootenum_pos *p = get_pos( e, thread, now_ms );

    if (p->index >= count)
    {
        /* end of the pass: forget the position, the next call starts a new pass */
        memset( p, 0, sizeof(*p) );
        return IOS_ROOTENUM_END;
    }

    p->last_used_ms = now_ms;
    *needed = sizes[p->index];
    if (sizes[p->index] <= size && (buffer || !sizes[p->index]))
    {
        if (sizes[p->index]) memcpy( buffer, data[p->index], sizes[p->index] );
        p->index++;
    }
    return IOS_ROOTENUM_OK;
}
