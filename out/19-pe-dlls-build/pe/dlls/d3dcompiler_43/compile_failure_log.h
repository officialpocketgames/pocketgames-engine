/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 (follow-up): what d3dcompiler logs when a compile fails, the pure part shared by
 * compiler.c and the host tests: how many failures a process reports, and which lines of the
 * compiler's messages go to the log.
 */

#ifndef MYIOSDECK_COMPILE_FAILURE_LOG_H
#define MYIOSDECK_COMPILE_FAILURE_LOG_H

#include <stddef.h>

#define COMPILE_FAIL_MAX_REPORTS  32    /* failures reported per process */
#define COMPILE_FAIL_MAX_LINES    12    /* message lines per report */
#define COMPILE_FAIL_MAX_BYTES    2048  /* message bytes per report */

enum compile_fail_action
{
    COMPILE_FAIL_REPORT,        /* log this failure */
    COMPILE_FAIL_LAST_NOTICE,   /* say that further failures aren't logged */
    COMPILE_FAIL_SILENT,        /* log nothing */
};

/* n: this failure's number in the process, from 1 */
static inline enum compile_fail_action compile_fail_action( long n )
{
    if (n >= 1 && n <= COMPILE_FAIL_MAX_REPORTS) return COMPILE_FAIL_REPORT;
    if (n == COMPILE_FAIL_MAX_REPORTS + 1) return COMPILE_FAIL_LAST_NOTICE;
    return COMPILE_FAIL_SILENT;
}

/* walks the message text (a blob: maybe not NUL-terminated, maybe with a NUL at its end) line by
 * line: empty lines are skipped, a trailing '\r' is dropped, and at most COMPILE_FAIL_MAX_LINES
 * lines and COMPILE_FAIL_MAX_BYTES bytes in all are returned (the last one cut to fit) */
struct compile_fail_lines
{
    const char *pos, *end;
    unsigned int lines;
    size_t bytes;
};

static inline void compile_fail_lines_init( struct compile_fail_lines *it, const void *text, size_t size )
{
    it->pos = text;
    it->end = text ? it->pos + size : it->pos;
    it->lines = 0;
    it->bytes = 0;
}

static inline int compile_fail_lines_next( struct compile_fail_lines *it, const char **line, int *len )
{
    while (it->pos < it->end && *it->pos)
    {
        const char *start = it->pos, *stop = start;
        size_t n;

        while (stop < it->end && *stop && *stop != '\n') stop++;
        it->pos = (stop < it->end && *stop == '\n') ? stop + 1 : stop;
        n = (size_t)(stop - start);
        if (n && start[n - 1] == '\r') n--;
        if (!n) continue;
        if (it->lines >= COMPILE_FAIL_MAX_LINES || it->bytes >= COMPILE_FAIL_MAX_BYTES) return 0;
        if (n > COMPILE_FAIL_MAX_BYTES - it->bytes) n = COMPILE_FAIL_MAX_BYTES - it->bytes;
        it->bytes += n;
        it->lines++;
        *line = start;
        *len = (int)n;
        return 1;
    }
    return 0;
}

#endif /* MYIOSDECK_COMPILE_FAILURE_LOG_H */
