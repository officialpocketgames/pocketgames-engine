// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 4: pure rules and formats (see ios_diag_pure.h).

#include <stdio.h>
#include <string.h>

#include "ios_diag_pure.h"

/* ---------- item 1.1 ---------- */

int ios_kill_on_server_gone( int has_teb, int leaving, int at_wait )
{
    return (has_teb && !leaving && !at_wait) ? IOS_KILL_FAIL : IOS_KILL_LEAVE;
}

int ios_kill_leave_at_boundary( int killed, int leaving, int nested )
{
    return killed && !leaving && !nested;
}

/* ---------- item 2 ---------- */

char *ios_format_syscall_fault( char *buf, size_t len, unsigned int serial, const char *exe,
                                unsigned int tid, unsigned int code, const char *name, unsigned int id,
                                const uint64_t args[4], uint64_t pc, const char *symbol, uint64_t offset,
                                uint64_t address, int count )
{
    char call[96], sym[160];

    if (!len) return buf;
    if (id == IOS_UNIX_CALL_ID) snprintf( call, sizeof(call), "a unix call (args" );
    else if (name) snprintf( call, sizeof(call), "system call %s (%#x, args", name, id );
    else snprintf( call, sizeof(call), "system call %#x (%#x, args", id, id );
    if (symbol) snprintf( sym, sizeof(sym), " (%s+%#llx)", symbol, (unsigned long long)offset );
    else sym[0] = 0;
    snprintf( buf, len, "process %u (%s), thread %04x: exception %x in %s %llx %llx %llx %llx) at pc "
              "%#llx%s, address %#llx: the call returns it as its status (fault %d of this run)\n",
              serial, exe ? exe : "", tid, code, call,
              (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2],
              (unsigned long long)args[3], (unsigned long long)pc, sym, (unsigned long long)address, count );
    return buf;
}

/* ---------- item 3 ---------- */

int ios_lookup_top3( const struct ios_lookup_stats *stats, int top[3] )
{
    int n = 0, i, j;

    for (i = 0; i < (int)stats->used; i++)
    {
        for (j = n; j > 0 && stats->paths[i].count > stats->paths[top[j - 1]].count; j--)
            if (j < 3) top[j] = top[j - 1];
        if (j < 3)
        {
            top[j] = i;
            if (n < 3) n++;
        }
    }
    return n;
}

static void count_path( struct ios_lookup_stats *stats, const char *path, unsigned int status )
{
    unsigned int i, least = 0;
    struct ios_lookup_path *p;

    for (i = 0; i < stats->used; i++)
        if (!strncmp( stats->paths[i].path, path, IOS_LOOKUP_PATH_LEN - 1 ))
        {
            stats->paths[i].count++;
            stats->paths[i].status = status;
            return;
        }
    if (stats->used < IOS_LOOKUP_PATHS)
    {
        p = &stats->paths[stats->used++];
        p->count = 1;
    }
    else
    {
        for (i = 1; i < IOS_LOOKUP_PATHS; i++)
            if (stats->paths[i].count < stats->paths[least].count) least = i;
        p = &stats->paths[least];
        p->count++;
    }
    snprintf( p->path, sizeof(p->path), "%s", path );
    p->status = status;
}

int ios_lookup_note( struct ios_lookup_stats *stats, uint64_t now_ms, const char *path,
                     unsigned int status, char *line, size_t len )
{
    int ret = 0, top[3], n, i, pos;

    if (stats->start_ms && now_ms - stats->start_ms >= IOS_LOOKUP_WINDOW_MS)
    {
        if (stats->count > IOS_LOOKUP_LIMIT && len)
        {
            n = ios_lookup_top3( stats, top );
            pos = snprintf( line, len, "%u lookups in 10 s; most often:", stats->count );
            for (i = 0; i < n && pos > 0 && (size_t)pos < len; i++)
                pos += snprintf( line + pos, len - pos, "%s %s (%u times, %#x)", i ? "," : "",
                                 stats->paths[top[i]].path, stats->paths[top[i]].count,
                                 stats->paths[top[i]].status );
            ret = 1;
        }
        stats->start_ms = 0;
    }
    if (!stats->start_ms)
    {
        stats->start_ms = now_ms;
        stats->count = 0;
        stats->used = 0;
    }
    stats->count++;
    count_path( stats, path ? path : "", status );
    return ret;
}
