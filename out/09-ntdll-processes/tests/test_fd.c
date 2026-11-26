// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 7 item 10: host tests of ios_fd.c (real descriptors of this process).

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ios_fd.h"

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf( stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond ); } } while (0)

static void test_limit_tries(void)
{
    uint64_t t[4];

    /* below kern.maxfilesperproc: that value, then 10240, 8192, 4096 */
    CHECK( ios_fd_limit_tries( 256, 24576, t ) == 4 );
    CHECK( t[0] == 24576 && t[1] == 10240 && t[2] == 8192 && t[3] == 4096 );
    /* only values above the soft limit */
    CHECK( ios_fd_limit_tries( 8192, 24576, t ) == 2 );
    CHECK( t[0] == 24576 && t[1] == 10240 );
    /* maxfiles equal to a fallback isn't tried twice */
    CHECK( ios_fd_limit_tries( 256, 10240, t ) == 3 );
    CHECK( t[0] == 10240 && t[1] == 8192 && t[2] == 4096 );
    /* already there, or nothing known: nothing to try */
    CHECK( ios_fd_limit_tries( 24576, 24576, t ) == 0 );
    CHECK( ios_fd_limit_tries( 30000, 24576, t ) == 0 );
    CHECK( ios_fd_limit_tries( 256, 0, t ) == 0 );
}

static void test_count(void)
{
    struct ios_fd_counts before, after, c;
    int p[3][2], s[2][2], f[2], i, limit = 4096;
    char line[200];

    ios_fd_count( &before, limit );
    for (i = 0; i < 3; i++) CHECK( !pipe( p[i] ) );
    for (i = 0; i < 2; i++) CHECK( !socketpair( AF_UNIX, SOCK_STREAM, 0, s[i] ) );
    CHECK( (f[0] = open( "/dev/null", O_RDONLY )) >= 0 );    /* a character device: other */
    CHECK( (f[1] = open( ".", O_RDONLY )) >= 0 );            /* a folder counts as a file */
    ios_fd_count( &after, limit );
    CHECK( after.open == before.open + 12 );
    CHECK( after.pipes == before.pipes + 6 );
    CHECK( after.sockets == before.sockets + 4 );
    CHECK( after.files == before.files + 1 );
    CHECK( after.other == before.other + 1 );
    for (i = 0; i < 3; i++) { close( p[i][0] ); close( p[i][1] ); }
    for (i = 0; i < 2; i++) { close( s[i][0] ); close( s[i][1] ); }
    close( f[0] );
    close( f[1] );
    ios_fd_count( &c, limit );
    CHECK( c.open == before.open );

    /* the lines */
    c.open = 9000; c.pipes = 6000; c.sockets = 2000; c.files = 900; c.other = 100;
    ios_fd_line( line, sizeof(line), &c, 10240, 126 );
    CHECK( !strcmp( line, "[fd] 9000 of 10240 open (6000 pipes, 2000 sockets, 900 files, 100 other), 126 threads" ) );
    ios_fd_largest_kinds( line, sizeof(line), &c );
    CHECK( !strcmp( line, "6000 pipes, 2000 sockets, 900 files, 100 other" ) );
    c.pipes = 10; c.sockets = 0; c.files = 40; c.other = 0;
    ios_fd_largest_kinds( line, sizeof(line), &c );
    CHECK( !strcmp( line, "40 files, 10 pipes" ) );
}

static void test_threshold_and_due(void)
{
    int done = 0;
    uint64_t last = 0;

    /* the 75% line once */
    CHECK( !ios_fd_threshold_once( &done, 7679, 10240 ) );
    CHECK( ios_fd_threshold_once( &done, 7680, 10240 ) );
    CHECK( !ios_fd_threshold_once( &done, 9000, 10240 ) );
    CHECK( !ios_fd_threshold_once( &done, 10240, 10240 ) );
    /* at most every 10 s */
    CHECK( ios_fd_due( &last, 5000 ) && last == 5000 );
    CHECK( !ios_fd_due( &last, 14999 ) );
    CHECK( ios_fd_due( &last, 15000 ) && last == 15000 );
}

int main(void)
{
    test_limit_tries();
    test_count();
    test_threshold_and_due();
    printf( "%d checks, %d failed\n", checks, failures );
    return failures != 0;
}
