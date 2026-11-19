// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 Part C: memory a 32-bit program can see, for the native Direct3D 9 frontend and its
// dispatch. Every pointer the native side hands back to a 32-bit program must be the guest
// address of memory inside that program's 4 GB guest window (spec 10 Part E: guest address a is
// host address ios_wow_base() + a). This allocator sub-allocates such memory from blocks of at
// least 8 MB that it reserves and commits with NtAllocateVirtualMemory on the current process
// (zero_bits 0x7fffffff, which spec 10 places in the window).
//
// Threads (spec 33 C2 and clarification 9):
//   - ios_wow_window_alloc runs only on a game thread inside a gate call (a Wine thread of the
//     32-bit process). It refuses (returns NULL) on a thread marked native and when the calling
//     thread has no guest window.
//   - ios_wow_window_free, ios_wow_window_owns and ios_wow_window_guest work on any thread,
//     including the frontend's native encode and finish threads. They never call Wine.
//   - A block that falls empty goes back to the system after 2 s unused. Giving it back needs a
//     Wine thread, so it happens in ios_wow_window_collect (and in ios_wow_window_alloc), on the
//     next game thread that calls either after the 2 s.

#ifndef MYIOSDECK_IOS_WOW_WINDOW_ALLOC_H
#define MYIOSDECK_IOS_WOW_WINDOW_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Smallest block the allocator reserves (C1). A larger request gets a block of its own size,
   rounded up to 64 KB, the Windows allocation granularity. */
#define IOS_WOW_WINDOW_BLOCK_MIN   (8u << 20)
/* Alignment of anything handed to Metal as a no-copy buffer (spec 06 clarification 1). */
#define IOS_WOW_WINDOW_METAL_ALIGN 16384u
/* How long an empty block stays before it goes back to the system (C1). */
#define IOS_WOW_WINDOW_RETURN_NS   2000000000ull

/* Returns the host address of `size` bytes of committed, zero-filled-on-first-use read-write
   memory inside the calling process's guest window, aligned to `align` (a power of two; 0 means
   16), and stores its guest address in *guest (when guest is not NULL). Returns NULL (and leaves
   *guest 0) on a native thread, on a thread with no guest window, for a bad alignment, or when
   the window has no room. Memory reused from an earlier free is not cleared. */
void *ios_wow_window_alloc( size_t size, size_t align, uint32_t *guest );

/* Returns memory ios_wow_window_alloc handed out. Any thread. NULL is ignored; so is (with one
   log line) a pointer the allocator does not own. */
void ios_wow_window_free( void *host );

/* 1 when host lies inside one of the allocator's blocks (so it came, or could have come, from
   ios_wow_window_alloc rather than from the native heap). Any thread. */
int ios_wow_window_owns( const void *host );

/* The guest address of host when it lies inside a block of this allocator, else 0. Any thread. */
uint32_t ios_wow_window_guest( const void *host );

/* Gives back empty blocks that have been unused for 2 s. Does nothing on a native thread or a
   thread with no guest window, and returns at once while no block is empty. */
void ios_wow_window_collect( void );

/* The platform layer marks each thread it creates (the encode, finish and event threads, the
   pipeline scheduler's workers) as native at its start: such a thread is not a Wine thread and
   never allocates memory the program can see. */
void ios_wow_window_mark_native_thread( void );
int ios_wow_window_is_native_thread( void );

/* Totals for logs and tests. */
struct ios_wow_window_stats
{
    uint32_t blocks;          /* blocks held */
    uint32_t empty_blocks;    /* of those, empty and waiting for their 2 s */
    uint64_t block_bytes;     /* bytes reserved in blocks */
    uint64_t used_bytes;      /* bytes handed out and not freed (after rounding) */
    uint32_t allocations;     /* live allocations */
};
void ios_wow_window_get_stats( struct ios_wow_window_stats *out );

#ifdef __cplusplus
}
#endif

#endif /* MYIOSDECK_IOS_WOW_WINDOW_ALLOC_H */
