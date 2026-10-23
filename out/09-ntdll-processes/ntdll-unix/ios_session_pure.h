// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 22 Part C: the pieces of "another program in the running session" that are pure rules
// or plain data structures. Only the C library, so tests/test_session_pure.c runs them on a host.

#ifndef MYIOSDECK_IOS_SESSION_PURE_H
#define MYIOSDECK_IOS_SESSION_PURE_H

/* ---------- C6: process-end hooks ---------- */

#define IOS_PROCESS_END_HOOKS_MAX 16

typedef void (*ios_process_end_fn)( const void *peb );

struct ios_hook_list
{
    ios_process_end_fn fn[IOS_PROCESS_END_HOOKS_MAX];   /* written once, before count covers it */
    int                count;                           /* published with release order */
    int                lock;                            /* adders only: 0 free, 1 taken */
};

/* Adds fn once. 0 when added, 1 when it was already there, -1 when fn is NULL or the list is full.
 * Any thread; running the list never waits for an adder. */
int ios_hook_list_add( struct ios_hook_list *list, ios_process_end_fn fn );

/* Calls every hook with peb, in the order they were added. Returns how many ran. */
int ios_hook_list_run( struct ios_hook_list *list, const void *peb );

/* ---------- C3: a killed main thread ends its process ---------- */

/* killed: the server ended the thread (its pipes closed); is_main: it booted its pseudo-process;
 * others: threads of the process besides it; ending: the process is already being ended.
 * 1 when the thread must end the whole process instead of leaving alone. */
int ios_killed_thread_ends_process( int killed, int is_main, int others, int ending );

/* ---------- C5: threads an ended process leaves behind ---------- */

enum ios_leftover_action
{
    IOS_LEFTOVER_GONE,      /* it left after the alert */
    IOS_LEFTOVER_SUSPEND,   /* blocked in a system call outside Wine, no Wine lock: suspended for good */
    IOS_LEFTOVER_LOCKED,    /* blocked while inside one of Wine's locked sections: kept */
    IOS_LEFTOVER_RUNNING,   /* runs (guest code): kept, and so is the process's memory */
};

/* gone: the thread has left; waiting: its Mach run state, read before any suspension, is
 * "waiting" (blocked in the kernel); in_wine: it is inside a section that holds a Wine lock
 * (or is half-way through leaving). */
enum ios_leftover_action ios_leftover_action( int gone, int waiting, int in_wine );

struct ios_leftover_counts
{
    int woken;       /* threads alerted after the end */
    int left;        /* of those, how many left on their own */
    int suspended;   /* suspended for good and forgotten */
    int kept;        /* still there (running or locked): the memory stays */
};

void ios_leftover_count( struct ios_leftover_counts *c, enum ios_leftover_action a );

/* ---------- C8: upstream's "previous exiting thread", one per pseudo-process ----------
 * A leaving thread takes the TEB the previous leaving thread of its own process left there (to
 * free it) and leaves its own (keep_self) or nothing. slot is that process's own static. */
void *ios_prev_teb_exchange( void **slot, void *self_teb, int keep_self );

#endif /* MYIOSDECK_IOS_SESSION_PURE_H */
