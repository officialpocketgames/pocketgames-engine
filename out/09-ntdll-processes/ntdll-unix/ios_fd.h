// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 7 item 10: the app's open-file table (one table: the wineserver runs in the app,
// so every pipe and socket between a thread and the server has both its ends in it). Plain POSIX
// (fstat, getrlimit, setrlimit), no Wine headers: the host tests use it as it is (ios_fd.c).

#ifndef MYIOSDECK_IOS_FD_H
#define MYIOSDECK_IOS_FD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct ios_fd_counts
{
    unsigned int open;
    unsigned int pipes;
    unsigned int sockets;
    unsigned int files;
    unsigned int other;
};

/* The values to try for the soft limit when it is below kern.maxfilesperproc (maxfiles):
 * maxfiles, then 10240, 8192 and 4096, each only when above the soft limit; returns how many
 * (at most 4) and fills tries in that order. None when the soft limit isn't below maxfiles. */
int ios_fd_limit_tries( uint64_t soft, uint64_t maxfiles, uint64_t tries[4] );

/* Counts one open descriptor by its st_mode */
void ios_fd_classify( struct ios_fd_counts *c, mode_t mode );

/* Counts the open descriptors 0..limit-1 (fstat; at most the first 65536) */
void ios_fd_count( struct ios_fd_counts *c, int limit );

/* "[fd] <open> of <limit> open (<pipes> pipes, <sockets> sockets, <files> files, <other>
 * other), <threads> threads"; returns buf */
char *ios_fd_line( char *buf, size_t len, const struct ios_fd_counts *c, unsigned int limit,
                   unsigned int threads );

/* The largest kinds, most first ("1504 pipes, 360 sockets, 41 files, 9 other", kinds with none
 * left out); returns buf */
char *ios_fd_largest_kinds( char *buf, size_t len, const struct ios_fd_counts *c );

/* 1 the first time open passes 75% of limit (and remembers it in *done), else 0 */
int ios_fd_threshold_once( int *done, unsigned int open, unsigned int limit );

/* 1 when 10 s have passed since *last_ms (or it is 0), and then *last_ms = now_ms */
int ios_fd_due( uint64_t *last_ms, uint64_t now_ms );

/* ---- the app side (ios_fd.c, not in the host tests' pure part) ---- */

/* At boot, after set_max_limit(RLIMIT_NOFILE): logs the limits and raises the soft one as
 * ios_fd_limit_tries says. log takes one line without its newline. */
void ios_fd_raise_limit( void (*log)( const char *line ));

/* The soft open-file limit (getrlimit), at least 1 */
unsigned int ios_fd_limit( void );

/* Spec 08's server, when receive_fd fails with EMSGSIZE or EMFILE: "[fd] the file table is full
 * (<open> of <limit>): <largest kinds>" into buf; returns buf. Exported for the server (weak). */
char *ios_fd_table_full_line( char *buf, size_t len );

#endif /* MYIOSDECK_IOS_FD_H */
