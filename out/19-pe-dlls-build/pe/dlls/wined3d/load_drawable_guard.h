/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 A23: the bookkeeping of wined3d_texture_load_drawable()'s re-entry guard, the pure
 * part shared by texture_gl.c and the host tests. A call registers its texture, sub-resource and
 * thread while it runs; a call for the same three while that one runs is a re-entry (the cycle
 * load_drawable -> blitters -> cpu blit -> load_location(DRAWABLE) -> load_drawable) and must
 * return at once. The caller holds a lock around enter and leave.
 */

#ifndef MYIOSDECK_LOAD_DRAWABLE_GUARD_H
#define MYIOSDECK_LOAD_DRAWABLE_GUARD_H

#define LOAD_DRAWABLE_GUARD_MAX 16  /* calls tracked at once (nested or on other threads) */
#define LOAD_DRAWABLE_REENTERED_MESSAGE \
    "[wined3d] load_drawable re-entered for the same sub-resource; frame not shown\n"

struct load_drawable_guard_entry
{
    const void *texture;
    unsigned int sub_resource_idx;
    unsigned long thread;
};

struct load_drawable_guard
{
    struct load_drawable_guard_entry entries[LOAD_DRAWABLE_GUARD_MAX];
    unsigned int count;
};

enum load_drawable_guard_result
{
    LOAD_DRAWABLE_REENTERED,    /* the same call is running: return at once */
    LOAD_DRAWABLE_ENTERED,      /* registered: call load_drawable_guard_leave() at the end */
    LOAD_DRAWABLE_UNTRACKED,    /* the table is full: go on, unregistered (no leave) */
};

static inline enum load_drawable_guard_result load_drawable_guard_enter(struct load_drawable_guard *guard,
        const void *texture, unsigned int sub_resource_idx, unsigned long thread)
{
    unsigned int i;

    for (i = 0; i < guard->count; ++i)
    {
        const struct load_drawable_guard_entry *e = &guard->entries[i];

        if (e->texture == texture && e->sub_resource_idx == sub_resource_idx && e->thread == thread)
            return LOAD_DRAWABLE_REENTERED;
    }
    if (guard->count == LOAD_DRAWABLE_GUARD_MAX)
        return LOAD_DRAWABLE_UNTRACKED;
    guard->entries[guard->count].texture = texture;
    guard->entries[guard->count].sub_resource_idx = sub_resource_idx;
    guard->entries[guard->count].thread = thread;
    ++guard->count;
    return LOAD_DRAWABLE_ENTERED;
}

/* removes the entry load_drawable_guard_enter() added (calls on other threads may have added
 * and removed others since, so it is searched, not popped) */
static inline void load_drawable_guard_leave(struct load_drawable_guard *guard,
        const void *texture, unsigned int sub_resource_idx, unsigned long thread)
{
    unsigned int i;

    for (i = guard->count; i-- > 0;)
    {
        struct load_drawable_guard_entry *e = &guard->entries[i];

        if (e->texture == texture && e->sub_resource_idx == sub_resource_idx && e->thread == thread)
        {
            guard->entries[i] = guard->entries[--guard->count];
            return;
        }
    }
}

#endif /* MYIOSDECK_LOAD_DRAWABLE_GUARD_H */
