// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 17: the prefix's own host name (common/ios_hostname.c) in a temporary
// directory, with a fake random source.

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ios_hostname.h"
#include "ios_test.h"

static char dir[256], path[512];

/* draws from a list, then counts up */
static const uint32_t *draws;
static size_t draw_count, draw_pos;
static uint32_t counter;

static uint32_t fake_random(void)
{
    if (draw_pos < draw_count) return draws[draw_pos++];
    return counter++;
}

static void set_draws( const uint32_t *list, size_t count, uint32_t start )
{
    draws = list;
    draw_count = count;
    draw_pos = 0;
    counter = start;
}

static void write_file( const char *data )
{
    int fd = open( path, O_WRONLY | O_CREAT | O_TRUNC, 0600 );

    if (fd == -1) return;
    if (write( fd, data, strlen( data ) ) < 0) { /* the test's checks catch it */ }
    close( fd );
}

static int read_file( char *buf, size_t size )
{
    int fd = open( path, O_RDONLY );
    ssize_t got;

    if (fd == -1) return -1;
    got = read( fd, buf, size - 1 );
    close( fd );
    if (got < 0) return -1;
    buf[got] = 0;
    return (int)got;
}

static void test_valid(void)
{
    CHECK( ios_hostname_valid( "DESKTOP-ABC1234", 15 ) );
    CHECK( ios_hostname_valid( "a", 1 ) );
    CHECK( ios_hostname_valid( "my-pc", 5 ) );
    CHECK( !ios_hostname_valid( "", 0 ) );
    CHECK( !ios_hostname_valid( "DESKTOP-ABC12345", 16 ) );   /* longer than NetBIOS allows */
    CHECK( !ios_hostname_valid( "-pc", 3 ) );
    CHECK( !ios_hostname_valid( "pc-", 3 ) );
    CHECK( !ios_hostname_valid( "Dan's iPhone", 12 ) );       /* a device name: not a host name */
    CHECK( !ios_hostname_valid( "pc.local", 8 ) );
    CHECK( !ios_hostname_valid( NULL, 3 ) );
}

static void test_make(void)
{
    static const uint32_t first[] = { 0, 25, 26, 35, 36, 71, 1 };
    /* the largest multiple of 36 that fits and above: drawn again */
    static const uint32_t biased[] = { 0xffffffffu, 0xfffffffcu, 2, 3, 4, 5, 6, 7, 8 };
    char name[IOS_HOSTNAME_LEN + 1];

    set_draws( first, 7, 0 );
    ios_hostname_make( name, fake_random );
    CHECK_STR( name, "DESKTOP-AZ09A9B" );
    CHECK( ios_hostname_valid( name, strlen( name ) ) );
    CHECK_EQ( strlen( name ), IOS_HOSTNAME_LEN );

    set_draws( biased, 9, 0 );
    ios_hostname_make( name, fake_random );
    CHECK_STR( name, "DESKTOP-CDEFGHI" );
    CHECK_EQ( draw_pos, 9 );
}

static void test_prefix_dir(void)
{
    char buf[64];

    CHECK( !ios_hostname_prefix_dir( "/var/prefix", "/home/u", buf, sizeof(buf) ) );
    CHECK_STR( buf, "/var/prefix" );
    CHECK( !ios_hostname_prefix_dir( "/var/prefix//", NULL, buf, sizeof(buf) ) );
    CHECK_STR( buf, "/var/prefix" );
    CHECK( !ios_hostname_prefix_dir( "/", NULL, buf, sizeof(buf) ) );
    CHECK_STR( buf, "/" );
    CHECK( !ios_hostname_prefix_dir( NULL, "/home/u", buf, sizeof(buf) ) );
    CHECK_STR( buf, "/home/u/.wine" );
    CHECK( !ios_hostname_prefix_dir( "", "/home/u", buf, sizeof(buf) ) );
    CHECK_STR( buf, "/home/u/.wine" );
    CHECK( ios_hostname_prefix_dir( "relative", "/home/u", buf, sizeof(buf) ) == -1 );
    CHECK( ios_hostname_prefix_dir( NULL, "relative", buf, sizeof(buf) ) == -1 );
    CHECK( ios_hostname_prefix_dir( NULL, NULL, buf, sizeof(buf) ) == -1 );
    CHECK( ios_hostname_prefix_dir( "/a/very/long/prefix/path", NULL, buf, 8 ) == -1 );
}

static void test_load(void)
{
    char name[IOS_HOSTNAME_LEN + 1], again[IOS_HOSTNAME_LEN + 1], buf[64];
    struct stat st;

    /* first start: no file, a new name is created with mode 0600 */
    unlink( path );
    set_draws( NULL, 0, 100 );
    CHECK_EQ( ios_hostname_load( dir, name, fake_random ), IOS_HOSTNAME_CREATED );
    CHECK( ios_hostname_valid( name, strlen( name ) ) );
    CHECK( !strncmp( name, IOS_HOSTNAME_PREFIX, 8 ) );
    CHECK_EQ( read_file( buf, sizeof(buf) ), IOS_HOSTNAME_LEN + 1 );
    CHECK( !strncmp( buf, name, IOS_HOSTNAME_LEN ) && buf[IOS_HOSTNAME_LEN] == '\n' );
    CHECK( !stat( path, &st ) && (st.st_mode & 0777) == 0600 );

    /* every later start: the same name, whatever the random source does */
    set_draws( NULL, 0, 7 );
    CHECK_EQ( ios_hostname_load( dir, again, fake_random ), IOS_HOSTNAME_READ );
    CHECK_STR( again, name );
    CHECK_EQ( draw_pos, 0 );
    CHECK_EQ( counter, 7 );   /* nothing drawn */

    /* a name set by hand, with CRLF and extra lines: its first line is used as it is */
    write_file( "my-pc\r\nignored\n" );
    CHECK_EQ( ios_hostname_load( dir, again, fake_random ), IOS_HOSTNAME_READ );
    CHECK_STR( again, "my-pc" );

    /* a file without a usable name: replaced by a new one, which then stays */
    write_file( "Dan's iPhone\n" );
    set_draws( NULL, 0, 200 );
    CHECK_EQ( ios_hostname_load( dir, name, fake_random ), IOS_HOSTNAME_REPLACED );
    CHECK( ios_hostname_valid( name, strlen( name ) ) );
    CHECK_EQ( ios_hostname_load( dir, again, fake_random ), IOS_HOSTNAME_READ );
    CHECK_STR( again, name );
    write_file( "" );
    CHECK_EQ( ios_hostname_load( dir, name, fake_random ), IOS_HOSTNAME_REPLACED );

    /* no prefix directory, or one that doesn't exist: a name for this run only */
    CHECK_EQ( ios_hostname_load( NULL, name, fake_random ), IOS_HOSTNAME_NOT_STORED );
    CHECK( ios_hostname_valid( name, strlen( name ) ) );
    CHECK_EQ( ios_hostname_load( "/nonexistent-dir-for-test", name, fake_random ), IOS_HOSTNAME_NOT_STORED );
    CHECK( ios_hostname_valid( name, strlen( name ) ) );
    unlink( path );
}

int main(void)
{
    const char *tmp = getenv( "TMPDIR" );

    snprintf( dir, sizeof(dir), "%s/hostname-test.XXXXXX", tmp && *tmp ? tmp : "/tmp" );
    if (!mkdtemp( dir ))
    {
        printf( "FAIL: no temporary directory\n" );
        return 1;
    }
    snprintf( path, sizeof(path), "%s/%s", dir, IOS_HOSTNAME_FILE );

    test_valid();
    test_make();
    test_prefix_dir();
    test_load();

    unlink( path );
    rmdir( dir );
    return ios_test_done( "test_hostname" );
}
