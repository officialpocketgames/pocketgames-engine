// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 7 item 10: the app's open-file table (see ios_fd.h).

#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "ios_fd.h"

int ios_fd_limit_tries( uint64_t soft, uint64_t maxfiles, uint64_t tries[4] )
{
    static const uint64_t fallbacks[3] = { 10240, 8192, 4096 };
    int n = 0, i;

    if (!maxfiles || soft >= maxfiles) return 0;
    tries[n++] = maxfiles;
    for (i = 0; i < 3; i++)
        if (fallbacks[i] > soft && fallbacks[i] != maxfiles) tries[n++] = fallbacks[i];
    return n;
}

void ios_fd_classify( struct ios_fd_counts *c, mode_t mode )
{
    c->open++;
    if (S_ISFIFO( mode )) c->pipes++;
    else if (S_ISSOCK( mode )) c->sockets++;
    else if (S_ISREG( mode ) || S_ISDIR( mode )) c->files++;
    else c->other++;
}

void ios_fd_count( struct ios_fd_counts *c, int limit )
{
    struct stat st;
    int fd;

    memset( c, 0, sizeof(*c) );
    if (limit > 65536) limit = 65536;   /* an unlimited table isn't walked to its end */
    for (fd = 0; fd < limit; fd++)
        if (!fstat( fd, &st )) ios_fd_classify( c, st.st_mode );
}

char *ios_fd_line( char *buf, size_t len, const struct ios_fd_counts *c, unsigned int limit,
                   unsigned int threads )
{
    if (len) snprintf( buf, len, "[fd] %u of %u open (%u pipes, %u sockets, %u files, %u other), %u threads",
                       c->open, limit, c->pipes, c->sockets, c->files, c->other, threads );
    return buf;
}

char *ios_fd_largest_kinds( char *buf, size_t len, const struct ios_fd_counts *c )
{
    const char *names[4] = { "pipes", "sockets", "files", "other" };
    unsigned int counts[4] = { c->pipes, c->sockets, c->files, c->other };
    int order[4] = { 0, 1, 2, 3 }, i, j, pos = 0;

    if (!len) return buf;
    buf[0] = 0;
    for (i = 1; i < 4; i++)   /* most first; ties keep this order */
        for (j = i; j > 0 && counts[order[j]] > counts[order[j - 1]]; j--)
        {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (i = 0; i < 4 && pos >= 0 && (size_t)pos < len; i++)
    {
        if (!counts[order[i]]) continue;
        pos += snprintf( buf + pos, len - pos, "%s%u %s", pos ? ", " : "", counts[order[i]], names[order[i]] );
    }
    return buf;
}

int ios_fd_threshold_once( int *done, unsigned int open, unsigned int limit )
{
    if (*done || (uint64_t)open * 4 < (uint64_t)limit * 3) return 0;
    *done = 1;
    return 1;
}

int ios_fd_due( uint64_t *last_ms, uint64_t now_ms )
{
    if (*last_ms && now_ms - *last_ms < 10000) return 0;
    *last_ms = now_ms;
    return 1;
}

/* ---------- the app side ---------- */

unsigned int ios_fd_limit( void )
{
    struct rlimit rl;

    if (getrlimit( RLIMIT_NOFILE, &rl ) || !rl.rlim_cur) return 1;
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 0x7fffffff) return 0x7fffffff;
    return (unsigned int)rl.rlim_cur;
}

void ios_fd_raise_limit( void (*log)( const char *line ))
{
    struct rlimit rl;
    unsigned int maxfiles = 0;
    uint64_t tries[4];
    char line[160];
    int n, i;

#ifdef __APPLE__
    size_t size = sizeof(maxfiles);
    if (sysctlbyname( "kern.maxfilesperproc", &maxfiles, &size, NULL, 0 )) maxfiles = 0;
#endif
    if (getrlimit( RLIMIT_NOFILE, &rl )) return;
    snprintf( line, sizeof(line), "[fd] open-file limit: %llu (hard %llu, kern.maxfilesperproc %u)",
              (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max, maxfiles );
    log( line );
    n = ios_fd_limit_tries( rl.rlim_cur, maxfiles, tries );
    for (i = 0; i < n; i++)
    {
        struct rlimit want = rl;

        want.rlim_cur = tries[i];
        if (!setrlimit( RLIMIT_NOFILE, &want ))
        {
            snprintf( line, sizeof(line), "[fd] open-file limit raised to %llu", (unsigned long long)tries[i] );
            log( line );
            return;
        }
    }
    if (n)
    {
        snprintf( line, sizeof(line), "[fd] open-file limit stays %llu: none of %d higher values held",
                  (unsigned long long)rl.rlim_cur, n );
        log( line );
    }
}

char *ios_fd_table_full_line( char *buf, size_t len )
{
    struct ios_fd_counts c;
    unsigned int limit = ios_fd_limit();
    char kinds[96];

    ios_fd_count( &c, (int)limit );
    if (len) snprintf( buf, len, "[fd] the file table is full (%u of %u): %s", c.open, limit,
                       ios_fd_largest_kinds( kinds, sizeof(kinds), &c ));
    return buf;
}
