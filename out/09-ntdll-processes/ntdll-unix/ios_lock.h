// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 4, item 1.2-1.3: the unix-side locks every pseudo-process shares, with owner
// tracking. Plain pthreads, no Wine headers: the host tests use it as it is (ios_lock.c).

#ifndef MYIOSDECK_IOS_LOCK_H
#define MYIOSDECK_IOS_LOCK_H

#include <pthread.h>

/* who is asking: Windows thread and process ids (0: none) and the pseudo-process serial */
struct ios_lock_who
{
    unsigned int tid;
    unsigned int pid;
    unsigned int serial;
};

typedef void (*ios_lock_who_fn)( struct ios_lock_who *who );
typedef void (*ios_lock_log_fn)( const char *line );

/* ntdll's identity and log functions, and how long a wait is before it is logged (ms, 0: 3000).
 * Without them the identity is all zeros and lines go to stderr. */
void ios_lock_setup( ios_lock_who_fn who, ios_lock_log_fn log, unsigned int wait_log_ms );

/* a name for a lock in the wait lines (a string that lives for the app run) */
void ios_lock_name_set( pthread_mutex_t *mutex, const char *name );

/* pthread_mutex_lock with owner tracking. A wait longer than the log time writes one line:
 * "[lock] <name> waited 3 s by thread <tid> of process <pid>; held by thread <tid> of process
 * <pid> (<running|ended|suspended>)". When the holder's pseudo-process has ended, the lock is
 * re-initialized (recursive, which non-recursive users can't tell apart) and taken by the
 * waiter: "[lock] <name> was held by an ended process: released". A live holder's lock is
 * never taken. name may be NULL (then the registered name, else the address). */
void ios_lock_take( pthread_mutex_t *mutex, const char *name );

/* pthread_mutex_unlock with owner tracking */
void ios_lock_give( pthread_mutex_t *mutex );

/* the pseudo-process ended: locks its threads still hold may be recovered */
void ios_lock_process_ended( unsigned int serial );

/* the thread was suspended for good: its locks say "suspended" until its process ends */
void ios_lock_thread_suspended( unsigned int serial, unsigned int tid );

/* tests: forget every owner record and name */
void ios_lock_reset( void );

#endif /* MYIOSDECK_IOS_LOCK_H */
