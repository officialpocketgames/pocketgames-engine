// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14: log lines of the media libraries. Every line goes to the Wine log through ntdll's
// __wine_dbg_output, so it lands next to the program's own debug output. Plain C only (no Wine
// headers), so the platform-decoder object and the host tests can use it.

#ifndef IOS_MEDIA_LOG_H
#define IOS_MEDIA_LOG_H

#include <stdarg.h>
#include <stdint.h>

/* One line "<prefix><message>\n" to the log, unconditionally. */
void iom_log( const char *prefix, const char *fmt, ... ) __attribute__((format(printf, 2, 3)));
void iom_logv( const char *prefix, const char *fmt, va_list args );

/* Environment switches, read on every call (callers cache them when it matters). */
int iom_env_is( const char *name, const char *value );      /* getenv(name) equals value */
int iom_env_set( const char *name );                        /* set at all (even empty) */
int iom_diag( void );                                       /* PG_DIAG=1, read once */

/* A per-app line cap: lines up to `max` are printed, then one "<prefix>... further lines
 * suppressed" line, then nothing. Lock-free; the struct must start zeroed. */
struct iom_cap
{
    volatile int32_t count;
};
/* 1: print the line; 0: drop it. Prints the "suppressed" line itself when the cap is hit. */
int iom_cap_take( struct iom_cap *cap, int max, const char *prefix );

/* The parser's dedupe (Part B3 rule 12): at most `IOM_DEDUPE_MAX` distinct lines per app; a
 * line identical to an earlier one is printed again only at its 2nd, 4th, 8th ... occurrence,
 * with the count. Pure (no I/O) so the host tests can check it. */
#define IOM_DEDUPE_MAX 64
struct iom_dedupe
{
    char *lines[IOM_DEDUPE_MAX];
    uint32_t counts[IOM_DEDUPE_MAX];
    unsigned int used;
};
/* Returns 1 to print the line; *count is its occurrence number (1 for the first time). A new
 * line that doesn't fit the table returns 0. Not thread safe: callers hold a lock. */
int iom_dedupe_check( struct iom_dedupe *table, const char *line, uint32_t *count );

#endif /* IOS_MEDIA_LOG_H */
