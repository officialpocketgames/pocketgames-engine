// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16: everything of out/16-sync-ios/src/ that the server (spec 08) and ntdll (spec 09) call,
// besides the cell table (ios_fastsync.h) and the userspace ntsync library (linux/ntsync.h).
// Plain C11, no Wine header, so the host tests include it as it is.

#ifndef MYIOSDECK_IOS_SYNC_H
#define MYIOSDECK_IOS_SYNC_H

#include <stddef.h>
#include <stdint.h>

#include "ios_fastsync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======== Part A: choosing the engine (src/ios_sync_config.c) ======== */

/* the tuning values: their defaults, always (spec 00 Part I: no settings file) */
struct ios_sync_settings
{
    int      inproc_sync;          /* inproc-sync: -1 unset */
    unsigned lock_spin;            /* sync-lock-spin (E6): 256 */
    unsigned spin_us;              /* sync-spin-us (E6): 0 */
    unsigned yield_streak;         /* yield-streak (G1): 256 */
    unsigned yield_sleep_us;       /* yield-sleep-us (G1): 100, 0 = never */
    int      eco;                  /* eco (G2): off at start */
    int      eco_qos;              /* eco-qos (G2): IOS_ECO_QOS_* */
    unsigned alert_spin_us;        /* alert-spin-us (G3): 0 */
};

#define IOS_ECO_QOS_UTILITY     0
#define IOS_ECO_QOS_BACKGROUND  1
#define IOS_ECO_QOS_INITIATED   2

/* the tuning values of this app run */
extern const struct ios_sync_settings *ios_sync_settings(void);

/* fastsync switches (A3-A6), from the environment */
enum ios_fs_mode
{
    IOS_FS_OFF,
    IOS_FS_ON,       /* 1, on, yes: armed from the first call */
    IOS_FS_AUTO,     /* auto: arms itself under load (D2) */
    IOS_FS_CELLS,    /* cells: zero-timeout peeks only, never armed */
};

struct ios_fs_config
{
    int      mode;           /* enum ios_fs_mode, PG_FASTSYNC */
    int      sem;            /* PG_FASTSYNC_SEM */
    int      pollpeek;       /* PG_FS_POLLPEEK (default on) */
    unsigned spin_us;        /* PG_FASTSYNC_SPIN_US: 0..1000, default 40 */
    unsigned cap_us;         /* PG_FASTSYNC_CAP_US: 50..50000, default 2000 */
    unsigned auto_reqs;      /* PG_FS_AUTO_REQS: at least 1000, default 20000 */
};

typedef const char *(*ios_getenv_fn)( const char *name );

extern void ios_fs_config_parse( struct ios_fs_config *c, ios_getenv_fn get );
/* the switches of this app run, read the first time either side needs them (A5) */
extern const struct ios_fs_config *ios_fs_config(void);
/* A5: a later session asks for something else: logged once, ignored */
extern void ios_fs_config_recheck(void);
/* A2: in-process ntsync for this inproc-sync value and PG_SYNC_SESSION */
extern int ios_usync_engine_decide( int inproc_sync, const char *session );
/* A2: the server's decision, made once (the first get_inproc_device_fd) and logged */
extern int ios_usync_engine_selected(void);
/* A5: a later session's environment asks for the other engine: logged once, ignored */
extern void ios_usync_engine_recheck(void);

/* ======== Part C: fastsync, server hooks (src/ios_fastsync_srv.c), server thread only ======== */

/* "-1" in a return value below means the cell is DISABLED: the server's own field rules */

/* C2: a cell for a new event / semaphore, or -1 (fastsync off, table full, not eligible) */
extern int ios_fs_srv_event_alloc( int manual, int signaled );
extern int ios_fs_srv_sem_alloc( unsigned int initial, unsigned int max );
extern void ios_fs_srv_free( int cell );
extern void ios_fs_srv_queue( int cell, int delta );        /* srv_waiters +1 / -1 */

/* C3: events */
extern int ios_fs_ev_state( int cell );                      /* 1 SET or CLAIMED, 0, -1 */
extern int ios_fs_ev_signaled( int cell );                   /* 1, 0, -1 */
extern int ios_fs_ev_satisfied( int cell );                  /* 0, -1 */
extern int ios_fs_ev_signal( int cell, int set, int *prev );  /* 0 (clients woken), -1 */
extern int ios_fs_ev_disable( int cell );                    /* previous signalled 0/1, -1 */
extern void ios_fs_ev_unclaim( int cell );                   /* C5 */

/* C4: semaphores */
#define IOS_FS_SEM_OK        0
#define IOS_FS_SEM_LIMIT     1
#define IOS_FS_SEM_DISABLED  (-1)
extern int ios_fs_sem_release( int cell, unsigned int count, unsigned int *prev );
extern int ios_fs_sem_take( int cell );                      /* 1 took, 0 empty, -1 */
extern int ios_fs_sem_give_back( int cell );                 /* 0, -1 (add to the field) */
extern int ios_fs_sem_disable( int cell );                   /* the count it held, -1 */
extern int ios_fs_sem_count( int cell );                     /* the count, -1 */

/* ======== Part D: fastsync, client helpers (src/ios_fastsync_cli.c) ======== */

/* D1: the handle -> cell cache, app-wide, keyed by (handle, process id) */
#define IOS_FS_CACHE_ENTRIES  2048
#define IOS_FS_NOT_FAST       (-1)

struct ios_fs_entry
{
    uint32_t handle;
    uint32_t pid;
    uint32_t access;
    uint32_t gen;
    int32_t  index;     /* cell index, or IOS_FS_NOT_FAST */
    uint32_t manual;
};

/* 1 and *out on a hit */
extern int ios_fs_cache_lookup( uint32_t handle, uint32_t pid, struct ios_fs_entry *out );
/* writers: callers that already block signals (an uninterrupted section) may pass 1 */
extern void ios_fs_cache_store( const struct ios_fs_entry *entry, int signals_blocked );
extern void ios_fs_cache_evict_handle( uint32_t handle, int signals_blocked );
extern void ios_fs_cache_evict_pid( uint32_t pid, int signals_blocked );
/* the learn reply ("type") checked and turned into an entry; 0 when it isn't a cell */
extern int ios_fs_reply_index( uint32_t type, int32_t *index, uint32_t *manual );
/* D1: a hit may take the fast path for an operation that needs these access rights */
extern int ios_fs_entry_usable( const struct ios_fs_entry *entry, uint32_t access );
/* process initialisation, first thread, once the server said whether the ntsync device is in
 * use: evicts the process id's entries, logs the A6 line (or that the other engine runs) */
extern void ios_fs_process_init( uint32_t pid, int device_in_use );

/* D2: gates */
extern int ios_fs_cells_on(void);      /* mode not off (and not disabled for the session) */
extern int ios_fs_armed(void);
extern void ios_fs_count_unarmed(void); /* auto mode: one fast-path entry found it unarmed */
extern void ios_fs_set_other_engine( int on ); /* the client was handed the ntsync device */
extern int ios_fs_other_engine(void);

struct ios_fs_auto
{
    uint64_t window_start_ns;
    uint64_t window_start_count;
    int      started;
};
/* one check (every 256th count): 1 when the window reached the threshold (arm) */
extern int ios_fs_auto_check( struct ios_fs_auto *st, uint64_t count, uint64_t now_ns,
                              unsigned int threshold, uint64_t *elapsed_ns, uint64_t *in_window );

/* D3-D6: operations on a cell, with the generation learnt */
#define IOS_FS_GO_SERVER  (-1)
/* set (1) or reset (0): 0 done (*prev_state, *wake_server), IOS_FS_GO_SERVER */
extern int ios_fs_cli_event_change( int cell, uint32_t gen, int set, int *prev_state, int *wake_server );
/* release: 0 done (*prev, *wake_server), 1 limit exceeded, IOS_FS_GO_SERVER */
extern int ios_fs_cli_sem_release( int cell, uint32_t gen, unsigned int count, unsigned int *prev,
                                   int *wake_server );
/* query: 0 done, IOS_FS_GO_SERVER */
extern int ios_fs_cli_sem_query( int cell, uint32_t gen, unsigned int *count, unsigned int *max );
/* try once: 1 took / signalled, 0 not signalled, IOS_FS_GO_SERVER (recycled, disabled, claimed) */
extern int ios_fs_cli_try( int cell, uint32_t gen );
/* D7: the peek: 1 when the cell reads RESET / empty with this generation */
extern int ios_fs_cli_peek_unsignaled( int cell, uint32_t gen );
/* signalled as a client sees it, with this generation */
extern int ios_fs_cli_reads_signaled( int cell, uint32_t gen );

/* D6: spin credit (app-wide) */
#define IOS_FS_CREDIT_MAX 64
struct ios_fs_spin
{
    int      credit;     /* 0..64 */
    unsigned probe;      /* counts waits at credit 0 */
};
/* the timed-spin budget for one wait; *probe tells whether it is a probe */
extern uint64_t ios_fs_spin_budget( struct ios_fs_spin *st, uint64_t ceiling_ns, int *probe );
extern void ios_fs_spin_result( struct ios_fs_spin *st, int success, int probe );

/* D6: the whole fast wait on a cell (spin, then park); budget_ns: the time the caller allows
 * (UINT64_MAX: infinite). 1 success, 0 fall through. *spent_ns: the time it took; *charge_cap_ns:
 * the most a relative timeout may be charged for it (ios_fs_remaining_100ns). */
extern int ios_fs_cli_wait( int cell, uint32_t gen, uint64_t budget_ns, uint64_t *spent_ns,
                            uint64_t *charge_cap_ns );

/* D6: what is left of a relative timeout (100 ns units, > 0) after elapsed_ns here, charging at
 * most charge_cap_ns; never negative, 0 only when all of it was used */
extern int64_t ios_fs_remaining_100ns( int64_t timeout_100ns, uint64_t elapsed_ns, uint64_t charge_cap_ns );

/* D7: per-thread poll counters */
struct ios_fs_peek
{
    unsigned polls;     /* zero-timeout polls seen */
    unsigned peeks;     /* polls answered by the peek */
};
/* 1 when this poll must go to the server (every 16th) */
extern int ios_fs_peek_to_server( struct ios_fs_peek *st );
/* the peek answered: 1 when the thread should yield now (every 64th) */
extern int ios_fs_peek_answered( struct ios_fs_peek *st );

/* D8: watchdog slices in seconds: 2, 8, 32, 60, 60, ... (prev 0: the first) */
extern unsigned int ios_fs_watchdog_next( unsigned int prev_seconds );

/* the clock of D2 and D6: CLOCK_UPTIME_RAW in nanoseconds (host tests: CLOCK_MONOTONIC) */
extern uint64_t ios_fs_now_ns(void);

/* ======== Part G: pacing threads (src/ios_sync_pace.c) ======== */

struct ios_yield_state
{
    uint64_t last_ns;
    unsigned streak;
};
/* G1: microseconds to sleep instead of yielding (0: yield) */
extern unsigned int ios_yield_step( struct ios_yield_state *st, uint64_t now_ns,
                                    unsigned int streak_limit, unsigned int sleep_us );
/* G1: NtYieldExecution's decision for the calling thread (logs the values once) */
extern unsigned int ios_yield_decide(void);

/* G2: eco mode */
extern void pg_set_eco( int on );
extern int pg_get_eco(void);
extern unsigned int ios_eco_generation(void);
/* the calling thread applies the current choice if its generation is older (cheap otherwise) */
extern void ios_eco_apply(void);

#define IOS_QOS_USER_INTERACTIVE  0
#define IOS_QOS_USER_INITIATED    1
#define IOS_QOS_UTILITY           2
#define IOS_QOS_BACKGROUND        3
/* the class for eco on/off and the eco-qos setting */
extern int ios_eco_qos_class( int on, int eco_qos );
/* 1 when a thread that applied *applied_gen must apply current_gen now (updates it) */
extern int ios_eco_must_apply( unsigned int *applied_gen, unsigned int current_gen, int on );
extern int ios_eco_last_class;

#ifdef __cplusplus
}
#endif

#endif /* MYIOSDECK_IOS_SYNC_H */
