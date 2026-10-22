// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B1: object handles that carry a magic value. A handle is never dereferenced to
// check it: it encodes a magic number, the object type, a slot index and the slot's generation,
// and is looked up in a table. A parser handle passed as a stream, a stale handle (its slot
// freed or reused) or a random number all fail the lookup.

#ifndef WG_HANDLES_H
#define WG_HANDLES_H

#include <stdint.h>

enum wg_handle_type
{
    WG_HANDLE_TRANSFORM = 1,
    WG_HANDLE_PARSER = 2,
    WG_HANDLE_STREAM = 3,
};

/* 0 when the table is full or out of memory */
uint64_t wgh_alloc( enum wg_handle_type type, void *object );
/* the object, or NULL when the handle isn't a live handle of that type */
void *wgh_get( uint64_t handle, enum wg_handle_type type );
/* frees the slot (later lookups of the handle fail); returns the object or NULL */
void *wgh_free( uint64_t handle, enum wg_handle_type type );

#endif /* WG_HANDLES_H */
