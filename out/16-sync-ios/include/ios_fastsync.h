// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part B: the table of shared cells that holds the state of events and semaphores, read
// and written by the server thread (spec 08) and by every Windows thread (spec 09's ntdll) of the
// app. Included by both sides; plain C11, no Wine header.
//
// A cell (32 bytes, two per cache line):
//   word         generation in bits 63..32, state in bits 31..0 (a signed 32-bit value)
//   srv_waiters  server wait-queue entries on the object
//   cli_waiters  client threads parked on the cell
//   manual       1 = manual-reset event
//   kind         IOS_CELL_EVENT or IOS_CELL_SEMAPHORE
//   max          semaphore maximum
//   reserved
//
// Every access to word is a 64-bit atomic on both sides, sequentially consistent. The futex
// address is the state half (the low 32 bits, offset 0 on little-endian).

#ifndef MYIOSDECK_IOS_FASTSYNC_H
#define MYIOSDECK_IOS_FASTSYNC_H

#include <stddef.h>
#include <stdint.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ios_fastsync.h: the futex address is the low half of the cell word, at offset 0 only on little-endian"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* B1: the table */
#define IOS_SYNC_CELLS 8192

struct ios_sync_cell
{
    uint64_t word;
    int32_t  srv_waiters;
    int32_t  cli_waiters;
    uint32_t manual;
    uint32_t kind;
    uint32_t max;
    uint32_t reserved;
} __attribute__((aligned(8)));

_Static_assert( sizeof(struct ios_sync_cell) == 32, "a cell is 32 bytes" );
_Static_assert( offsetof(struct ios_sync_cell, word) == 0, "word is at offset 0" );

extern struct ios_sync_cell ios_sync_cells[IOS_SYNC_CELLS];

/* kinds */
#define IOS_CELL_EVENT      0u
#define IOS_CELL_SEMAPHORE  1u

/* B3: states. Event: DISABLED, RESET, SET, CLAIMED. Semaphore: the count, or DISABLED. */
#define IOS_CELL_DISABLED  (-1)
#define IOS_CELL_RESET     0
#define IOS_CELL_SET       1
#define IOS_CELL_CLAIMED   2

/* B7 / C7: the learn reply ("type" of get_inproc_sync_fd) */
#define IOS_FASTSYNC_REPLY_CELL    0x40000000u
#define IOS_FASTSYNC_REPLY_MANUAL  0x20000000u
#define IOS_FASTSYNC_INDEX_MASK    0x00ffffffu

/* B7 / C7: event_op codes outside upstream's enum */
#define IOS_EVENT_OP_WAKE               0x10001
#define IOS_EVENT_OP_DISABLE_EVENT      0x10002
#define IOS_EVENT_OP_DISABLE_SEMAPHORE  0x10003

/* ---- the word ---- */

static inline uint64_t ios_cell_make( uint32_t gen, int32_t state )
{
    return ((uint64_t)gen << 32) | (uint32_t)state;
}

static inline uint32_t ios_cell_gen( uint64_t word )
{
    return (uint32_t)(word >> 32);
}

static inline int32_t ios_cell_state( uint64_t word )
{
    return (int32_t)(uint32_t)word;
}

static inline struct ios_sync_cell *ios_cell_at( int index )
{
    return &ios_sync_cells[index];
}

static inline uint64_t ios_cell_load( const struct ios_sync_cell *cell )
{
    return __atomic_load_n( &cell->word, __ATOMIC_SEQ_CST );
}

static inline void ios_cell_store( struct ios_sync_cell *cell, uint64_t word )
{
    __atomic_store_n( &cell->word, word, __ATOMIC_SEQ_CST );
}

/* compare-and-swap: on failure *expected holds the current word */
static inline int ios_cell_cas( struct ios_sync_cell *cell, uint64_t *expected, uint64_t desired )
{
    return __atomic_compare_exchange_n( &cell->word, expected, desired, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static inline int32_t ios_cell_srv_waiters( const struct ios_sync_cell *cell )
{
    return __atomic_load_n( &cell->srv_waiters, __ATOMIC_SEQ_CST );
}

static inline int32_t ios_cell_cli_waiters( const struct ios_sync_cell *cell )
{
    return __atomic_load_n( &cell->cli_waiters, __ATOMIC_SEQ_CST );
}

static inline void ios_cell_add_cli_waiters( struct ios_sync_cell *cell, int32_t delta )
{
    __atomic_add_fetch( &cell->cli_waiters, delta, __ATOMIC_SEQ_CST );
}

/* "signalled" as a client sees it (B3): semaphore count > 0; manual event > 0; auto event SET
 * (a CLAIMED token belongs to the server) */
static inline int ios_cell_client_signaled( uint32_t kind, uint32_t manual, int32_t state )
{
    if (kind == IOS_CELL_SEMAPHORE) return state > 0;
    if (manual) return state > 0;
    return state == IOS_CELL_SET;
}

/* ---- B5: park and wake (src/ios_sync_cells.c) ---- */

/* wait while the state half still holds "state", at most timeout_ns (> 0) nanoseconds on the
 * Mach absolute clock; returns 0 when woken or the value differed, -1 with errno (ETIMEDOUT) */
extern int ios_cell_park( struct ios_sync_cell *cell, int32_t state, uint64_t timeout_ns );
/* wake one or all threads parked on the cell */
extern void ios_cell_wake( struct ios_sync_cell *cell, int all );

/* ---- C1: allocation, server thread only (src/ios_sync_cells.c) ---- */

/* a cell for a new object, or -1 when the table is full (logged once) */
extern int ios_cell_alloc( uint32_t kind, uint32_t manual, uint32_t max, int32_t state );
extern void ios_cell_free( int index );
/* 1 once any cell was ever allocated in this app run */
extern int ios_cells_in_use(void);

/* ---- logging: one line to stderr (the app points it at its log) ---- */
extern void ios_sync_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* MYIOSDECK_IOS_FASTSYNC_H */
