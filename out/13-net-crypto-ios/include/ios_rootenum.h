// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part F2: every enumeration of the root certificates sees every root.
//
// One list of roots serves every program. Each calling thread has its own position in it (the
// Windows side's root sync runs on one thread):
//   - a call returns the certificate at the position and its size in *needed; when the buffer
//     is too small the position doesn't move (the caller grows the buffer and asks again);
//     after a copy it moves to the next;
//   - at the end of the list the call returns IOS_ROOTENUM_END and forgets the position, so the
//     thread's next call starts a new pass from the first certificate;
//   - a position unused for IOS_ROOTENUM_IDLE_MS starts over (a pass abandoned halfway);
//   - IOS_ROOTENUM_SLOTS positions; a new thread takes a free one or the one used longest ago.
// The caller holds one lock around every call (crypt32/unixlib.c). Pure: no clock, no thread
// calls, no allocation, so the host tests drive it with fake threads and times.

#ifndef MYIOSDECK_IOS_ROOTENUM_H
#define MYIOSDECK_IOS_ROOTENUM_H

#include <stddef.h>
#include <stdint.h>

#define IOS_ROOTENUM_SLOTS   32
#define IOS_ROOTENUM_IDLE_MS 10000

enum ios_rootenum_result
{
    IOS_ROOTENUM_OK  = 0,   /* *needed is the certificate's size; copied if it fit */
    IOS_ROOTENUM_END = 1,   /* no more certificates in this pass */
};

struct ios_rootenum_pos
{
    uint64_t thread;        /* the thread this position belongs to */
    uint64_t last_used_ms;  /* time of its last call */
    size_t   index;         /* next certificate to return */
    int      in_use;
};

struct ios_rootenum
{
    struct ios_rootenum_pos pos[IOS_ROOTENUM_SLOTS];
};

/* All-zero is the initial state. */

/* One call of the enumeration for thread at time now_ms over the certificates
 * data[0..count) of sizes[0..count). Copies into buffer when sizes[index] <= size. */
extern enum ios_rootenum_result ios_rootenum_next( struct ios_rootenum *e, uint64_t thread,
                                                   uint64_t now_ms, size_t count,
                                                   const size_t *sizes,
                                                   const unsigned char *const *data,
                                                   void *buffer, size_t size, size_t *needed );

/* the slot of thread, or -1 (for tests and logging) */
extern int ios_rootenum_slot_of( const struct ios_rootenum *e, uint64_t thread );

#endif /* MYIOSDECK_IOS_ROOTENUM_H */
