// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 4: pure rules and formats (ios_diag_pure.c), with host tests.

#ifndef MYIOSDECK_IOS_DIAG_PURE_H
#define MYIOSDECK_IOS_DIAG_PURE_H

#include <stddef.h>
#include <stdint.h>

/* ---------- item 1.1: a killed thread finishes its call ----------
 * What a server request does when the server has closed the calling thread's pipes (it was
 * killed): IOS_KILL_FAIL (the request fails with STATUS_THREAD_IS_TERMINATING and the thread
 * leaves at its next system-call boundary) for a Windows thread that isn't leaving yet and isn't
 * at a wait for the server; IOS_KILL_LEAVE (leave now, as before) otherwise. */
enum { IOS_KILL_FAIL, IOS_KILL_LEAVE };
int ios_kill_on_server_gone( int has_teb, int leaving, int at_wait );

/* At a system-call boundary (entry or return in the dispatcher): 1 when the thread leaves there.
 * nested: the call runs inside a user callback, so an outer call isn't finished yet. */
int ios_kill_leave_at_boundary( int killed, int leaving, int nested );

/* ---------- item 2: a fault inside a system call names the call ----------
 * The line for a unix-side fault that the call returns as its status. name may be NULL (the
 * number is shown), symbol may be NULL (no "(symbol+offset)"). id IOS_UNIX_CALL_ID: the frame
 * belongs to a unix call (the dispatcher marks it), shown as "a unix call (args ...)". Returns buf. */
#define IOS_UNIX_CALL_ID 0xffffffffu
char *ios_format_syscall_fault( char *buf, size_t len, unsigned int serial, const char *exe,
                                unsigned int tid, unsigned int code, const char *name, unsigned int id,
                                const uint64_t args[4], uint64_t pc, const char *symbol, uint64_t offset,
                                uint64_t address, int count );

/* ---------- item 3: tight file-lookup loops ----------
 * One thread's lookups (NtQueryAttributesFile, NtQueryFullAttributesFile, NtOpenFile) in a window
 * of 10 s. The paths are counted in a table of IOS_LOOKUP_PATHS entries; when it is full the
 * least counted entry gives way and the new path starts from that count + 1 (exact for loops over
 * fewer paths than the table holds). */
#define IOS_LOOKUP_PATHS     32
#define IOS_LOOKUP_PATH_LEN  200
#define IOS_LOOKUP_WINDOW_MS 10000
#define IOS_LOOKUP_LIMIT     2000

struct ios_lookup_path
{
    char         path[IOS_LOOKUP_PATH_LEN];
    unsigned int count;
    unsigned int status;     /* of its latest lookup */
};

struct ios_lookup_stats
{
    uint64_t               start_ms;   /* of the window; 0: none yet */
    unsigned int           count;
    unsigned int           used;
    struct ios_lookup_path paths[IOS_LOOKUP_PATHS];
};

/* Counts one lookup at now_ms (never 0). When it starts a new window and the window before it
 * had more than IOS_LOOKUP_LIMIT lookups, writes the line for that window into line (without the
 * thread and process prefix: "<n> lookups in 10 s; most often: <path> (<k> times, <status>), ...")
 * and returns 1; else 0. */
int ios_lookup_note( struct ios_lookup_stats *stats, uint64_t now_ms, const char *path,
                     unsigned int status, char *line, size_t len );

/* The indexes of the (up to) three most counted paths, most first (ties: the earlier entry);
 * returns how many. */
int ios_lookup_top3( const struct ios_lookup_stats *stats, int top[3] );

#endif /* MYIOSDECK_IOS_DIAG_PURE_H */
