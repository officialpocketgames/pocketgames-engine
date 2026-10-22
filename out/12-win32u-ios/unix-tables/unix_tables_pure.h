// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarifications 8 and 9: the "log once per pseudo-process" rule of the stand-in unix
// tables, plain C (no Wine headers) so tests/test_unix_tables.c runs it on a host.

#ifndef MYIOSDECK_UNIX_TABLES_PURE_H
#define MYIOSDECK_UNIX_TABLES_PURE_H

#include <stdint.h>

#define W32IOS_UNIX_SEEN 32   /* processes remembered per library; the oldest is forgotten after that */

/* The pseudo-processes, by (PEB, process id), that have logged for one library.
 * Zero-initialised is empty. */
struct w32ios_unix_seen
{
    const void  *peb[W32IOS_UNIX_SEEN];
    uint32_t     pid[W32IOS_UNIX_SEEN];
    unsigned int count;   /* entries in use, at most W32IOS_UNIX_SEEN */
    unsigned int next;    /* the entry written next once full */
};

/* 1 when (peb, pid) isn't remembered: it is then remembered (overwriting the oldest when full)
 * and the caller logs. 0 when it is. The caller serialises calls. */
int w32ios_unix_first_report( struct w32ios_unix_seen *seen, const void *peb, uint32_t pid );

#endif
