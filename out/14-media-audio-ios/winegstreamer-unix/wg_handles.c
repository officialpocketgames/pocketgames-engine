// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B1: the handle table (see wg_handles.h).
//
// Layout of a handle: bits 63-48 magic 0x5747 ("WG"), 47-40 type, 39-20 generation (20 bits,
// never 0), 19-0 slot index + 1.

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "wg_handles.h"

#define WGH_MAGIC 0x5747ull
#define WGH_MAX_SLOTS ((1u << 20) - 1)
#define WGH_GEN_MASK 0xfffffu

struct slot
{
    void *object;       /* NULL: free */
    uint32_t gen;
    uint8_t type;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct slot *slots;
static uint32_t slot_count, slot_cap;

static uint64_t encode( uint32_t index, const struct slot *s )
{
    return (WGH_MAGIC << 48) | ((uint64_t)s->type << 40) | ((uint64_t)s->gen << 20) | (index + 1);
}

static struct slot *lookup( uint64_t handle, enum wg_handle_type type, uint32_t *index )
{
    uint32_t i = (uint32_t)(handle & 0xfffff), gen = (uint32_t)((handle >> 20) & WGH_GEN_MASK);

    if ((handle >> 48) != WGH_MAGIC || ((handle >> 40) & 0xff) != (uint64_t)type || !i) return NULL;
    i--;
    if (i >= slot_count) return NULL;
    if (!slots[i].object || slots[i].gen != gen || slots[i].type != type) return NULL;
    *index = i;
    return &slots[i];
}

uint64_t wgh_alloc( enum wg_handle_type type, void *object )
{
    uint64_t handle = 0;
    uint32_t i;

    if (!object) return 0;
    pthread_mutex_lock( &lock );
    for (i = 0; i < slot_count; i++) if (!slots[i].object) break;
    if (i == slot_count)
    {
        if (slot_count == slot_cap)
        {
            uint32_t cap = slot_cap ? slot_cap * 2 : 64;
            struct slot *p;

            if (cap > WGH_MAX_SLOTS) cap = WGH_MAX_SLOTS;
            if (cap <= slot_cap || !(p = realloc( slots, cap * sizeof(*p) ))) goto done;
            memset( p + slot_cap, 0, (cap - slot_cap) * sizeof(*p) );
            slots = p;
            slot_cap = cap;
        }
        slot_count++;
    }
    slots[i].gen = (slots[i].gen + 1) & WGH_GEN_MASK;
    if (!slots[i].gen) slots[i].gen = 1;
    slots[i].type = (uint8_t)type;
    slots[i].object = object;
    handle = encode( i, &slots[i] );
done:
    pthread_mutex_unlock( &lock );
    return handle;
}

void *wgh_get( uint64_t handle, enum wg_handle_type type )
{
    struct slot *s;
    uint32_t i;
    void *ret = NULL;

    pthread_mutex_lock( &lock );
    if ((s = lookup( handle, type, &i ))) ret = s->object;
    pthread_mutex_unlock( &lock );
    return ret;
}

void *wgh_free( uint64_t handle, enum wg_handle_type type )
{
    struct slot *s;
    uint32_t i;
    void *ret = NULL;

    pthread_mutex_lock( &lock );
    if ((s = lookup( handle, type, &i )))
    {
        ret = s->object;
        s->object = NULL;
    }
    pthread_mutex_unlock( &lock );
    return ret;
}
