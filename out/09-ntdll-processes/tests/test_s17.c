// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Part J, Parts B, C and D: host tests of ntdll-unix/ios_s17_pure.c and ios_s17.c
// (built against Wine's headers with this tree's overrides; ios_log is a fake that counts and
// keeps the last line).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"
#include "unixlib.h"

#include "ios_s17.h"
#include "test_sig_check.h"

#define CHECK_STR(a, b) do { checks++; if (strcmp( (a), (b) )) { failures++; \
    printf( "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b) ); } } while (0)

/* ---- fake ios_log ---- */

static int log_lines;
static char last_log[1024];

void ios_log( const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( last_log, sizeof(last_log), fmt, args );
    va_end( args );
    log_lines++;
}

/* ---- a scratch directory ---- */

static char dir[256];

static void make_dir(void)
{
    snprintf( dir, sizeof(dir), "%s/s17test.XXXXXX", getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "/tmp" );
    if (!mkdtemp( dir )) { perror( "mkdtemp" ); exit( 2 ); }
}

static void path_of( char *buf, size_t size, const char *name )
{
    snprintf( buf, size, "%s/%s", dir, name );
}

static void write_file( const char *name, const char *text, size_t len )
{
    char path[512];
    int fd;

    path_of( path, sizeof(path), name );
    fd = open( path, O_WRONLY | O_CREAT | O_TRUNC, 0644 );
    if (fd == -1 || write( fd, text, len ) != (ssize_t)len) { perror( path ); exit( 2 ); }
    close( fd );
}

static int read_file( const char *name, char *buf, size_t size )
{
    char path[512];
    int fd, n;

    path_of( path, sizeof(path), name );
    if ((fd = open( path, O_RDONLY )) == -1) return -1;
    n = (int)read( fd, buf, size - 1 );
    close( fd );
    if (n >= 0) buf[n] = 0;
    return n;
}

static void remove_file( const char *name )
{
    char path[512];
    path_of( path, sizeof(path), name );
    unlink( path );
}

/* ---------------------------------------------------------------- Part B */

static void test_part_b(void)
{
    const unsigned long B = 0x300000000ul, CEIL = 0xfffeffff;
    uint64_t map[4] = { 0 };

    /* switches: on unless exactly "0" */
    CHECK( ios_s17_switch_value( NULL ) );
    CHECK( ios_s17_switch_value( "" ) );
    CHECK( ios_s17_switch_value( "1" ) );
    CHECK( ios_s17_switch_value( "00" ) );
    CHECK( !ios_s17_switch_value( "0" ) );

    /* B1 is_wow64(teb switch, thread has a 32-bit TEB, process-wide) */
    CHECK( !ios_s17_is_wow64( 1, 0, 0 ) );   /* 64-bit thread */
    CHECK( ios_s17_is_wow64( 1, 1, 0 ) );    /* 32-bit thread, process-wide answer changed */
    CHECK( ios_s17_is_wow64( 1, 1, 1 ) );    /* 32-bit thread */
    CHECK( ios_s17_is_wow64( 1, 0, 1 ) );    /* a TEB-less thread: the process-wide answer */
    CHECK( !ios_s17_is_wow64( 0, 1, 0 ) );   /* PG_WOW64_BY_TEB=0 */
    CHECK( ios_s17_is_wow64( 0, 1, 1 ) );

    /* B2 in_wow64_call(owner switch, window base, upstream) */
    CHECK( !ios_s17_in_wow64_call( 1, 0, 0 ) );   /* 64-bit thread */
    CHECK( ios_s17_in_wow64_call( 1, B, 1 ) );    /* 32-bit thread */
    CHECK( !ios_s17_in_wow64_call( 1, 0, 1 ) );   /* 64-bit process while a 32-bit one exists */
    CHECK( ios_s17_in_wow64_call( 1, B, 0 ) );    /* 32-bit process, stale global */
    CHECK( ios_s17_in_wow64_call( 0, 0, 1 ) );    /* PG_IO_STATUS_OWNER=0: upstream's */
    CHECK( !ios_s17_in_wow64_call( 0, B, 0 ) );

    /* B4 section ceiling(per-process switch, window base, guest ceiling) */
    CHECK_EQ( ios_s17_section_limit( 1, 0, 0 ), 0 );          /* 64-bit process */
    CHECK_EQ( ios_s17_section_limit( 1, B, CEIL ), CEIL );    /* 32-bit process */
    CHECK_EQ( ios_s17_section_limit( 1, 0, CEIL ), 0 );       /* 64-bit helper after a 32-bit one */
    CHECK_EQ( ios_s17_section_limit( 0, 0, CEIL ), CEIL );    /* PG_SECTION_PROCESS_LIMIT=0 */
    CHECK_EQ( ios_s17_section_limit( 0, B, CEIL ), CEIL );

    /* B3 is_ec_code: NULL bitmap is FALSE, never a dereference */
    CHECK( !ios_s17_ec_bit( NULL, 0x140001000, 0x1000 ) );
    map[1] = 1ull << 3;   /* page 67 */
    CHECK( ios_s17_ec_bit( map, 67 * 0x1000 + 0x123, 0x1000 ) );
    CHECK( !ios_s17_ec_bit( map, 66 * 0x1000, 0x1000 ) );
    CHECK( !ios_s17_ec_bit( map, 68 * 0x1000, 0x1000 ) );
}

/* ---------------------------------------------------------------- Part C1 */

static uint32_t fixed_values[8];
static int fixed_next;

static uint32_t fixed_random(void)
{
    return fixed_values[fixed_next++ & 7];
}

static void test_serial_hash(void)
{
    CHECK_EQ( ios_s17_volume_serial( 0x12345678, 'c' ), 0x1d5b1f12 );
    CHECK_EQ( ios_s17_volume_serial( 0xdeadbeef, 'c' ), 0x2d805578 );
    CHECK_EQ( ios_s17_volume_serial( 0x12345678, 0 ), 0xbc5a865f );
    CHECK_EQ( ios_s17_volume_serial( 0xdeadbeef, 0 ), 0x50808c91 );
    CHECK_EQ( ios_s17_volume_serial( 0, 'c' ), 0 );   /* no stored value */
    CHECK( ios_s17_volume_serial( 0x12345678, 'c' ) != ios_s17_volume_serial( 0x12345678, 'd' ) );
}

/* a stored value whose hash with some letter is 0 or 0xffffffff becomes the stored value: find
 * such a (value, letter) by inverting FNV-1a's last step */
static void test_serial_substitution(void)
{
    static const uint32_t targets[2] = { 0, 0xffffffffu };
    const uint32_t prime_inverse = 0x359c449bu;   /* 16777619 * 0x359c449b == 1 (mod 2^32) */
    uint32_t stored = 0x0badf00d;
    int t;

    CHECK_EQ( (uint32_t)(16777619u * prime_inverse), 1 );
    for (t = 0; t < 2; t++)
    {
        /* the hash before the letter byte, from the stored value's four bytes */
        uint32_t h = 2166136261u, before_mul;
        int i, found = 0;

        for (i = 0; i < 4; i++) { h ^= (stored >> (8 * i)) & 0xff; h *= 16777619u; }
        before_mul = targets[t] * prime_inverse;   /* (h ^ letter) must equal this */
        if (((h ^ before_mul) & ~0xffu) == 0)
        {
            unsigned char letter = (unsigned char)(h ^ before_mul);
            CHECK_EQ( ios_s17_volume_serial( stored, letter ), stored );
            found = 1;
        }
        if (!found)
        {
            /* search stored values until the low byte works out */
            uint32_t s;
            for (s = 1; s && !found; s++)
            {
                h = 2166136261u;
                for (i = 0; i < 4; i++) { h ^= (s >> (8 * i)) & 0xff; h *= 16777619u; }
                if (((h ^ before_mul) & ~0xffu) == 0)
                {
                    unsigned char letter = (unsigned char)(h ^ before_mul);
                    CHECK_EQ( ios_s17_volume_serial( s, letter ), s );
                    found = 1;
                }
            }
        }
        CHECK( found );
    }
}

static void test_serial_parse(void)
{
    CHECK_EQ( ios_s17_parse_serial( "12345678\n", 9 ), 0x12345678 );
    CHECK_EQ( ios_s17_parse_serial( "deadbeef", 8 ), 0xdeadbeef );     /* no newline */
    CHECK_EQ( ios_s17_parse_serial( "DEADBEEF\n", 9 ), 0xdeadbeef );
    CHECK_EQ( ios_s17_parse_serial( "00000000\n", 9 ), 0 );
    CHECK_EQ( ios_s17_parse_serial( "zzzz\n", 5 ), 0 );                /* garbage */
    CHECK_EQ( ios_s17_parse_serial( "", 0 ), 0 );
    CHECK_EQ( ios_s17_parse_serial( "1ffffffff", 9 ), 0 );             /* more than 32 bits */
    /* at most 15 bytes are looked at */
    CHECK_EQ( ios_s17_parse_serial( "0000000000000012345", 19 ), 0x1 );
}

static void test_serial_file(void)
{
    char buf[64];
    struct stat st;
    char path[512];

    make_dir();
    path_of( path, sizeof(path), IOS_S17_SERIAL_FILE );

    /* no config dir */
    CHECK_EQ( ios_s17_load_serial( NULL, fixed_random ), 0 );
    CHECK_EQ( ios_s17_load_serial( "", fixed_random ), 0 );

    /* existing file, with and without newline */
    write_file( IOS_S17_SERIAL_FILE, "12345678\n", 9 );
    CHECK_EQ( ios_s17_load_serial( dir, fixed_random ), 0x12345678 );
    write_file( IOS_S17_SERIAL_FILE, "deadbeef", 8 );
    CHECK_EQ( ios_s17_load_serial( dir, fixed_random ), 0xdeadbeef );

    /* missing file: draw (skipping 0 and 0xffffffff) and create it, 0600, "%08x\n" */
    remove_file( IOS_S17_SERIAL_FILE );
    fixed_values[0] = 0; fixed_values[1] = 0xffffffffu; fixed_values[2] = 0x00c0ffee; fixed_next = 0;
    CHECK_EQ( ios_s17_load_serial( dir, fixed_random ), 0x00c0ffee );
    CHECK_EQ( fixed_next, 3 );
    CHECK_EQ( read_file( IOS_S17_SERIAL_FILE, buf, sizeof(buf) ), 9 );
    CHECK_STR( buf, "00c0ffee\n" );
    CHECK( stat( path, &st ) == 0 && (st.st_mode & 0777) == 0600 );
    /* the next run reads it back */
    fixed_next = 0; fixed_values[0] = 0x11111111;
    CHECK_EQ( ios_s17_load_serial( dir, fixed_random ), 0x00c0ffee );
    CHECK_EQ( fixed_next, 0 );

    /* a file holding 0, or garbage, is replaced like a missing one */
    write_file( IOS_S17_SERIAL_FILE, "00000000\n", 9 );
    fixed_next = 0; fixed_values[0] = 0x22222222;
    CHECK_EQ( ios_s17_load_serial( dir, fixed_random ), 0x22222222 );
    CHECK_EQ( read_file( IOS_S17_SERIAL_FILE, buf, sizeof(buf) ), 9 );
    CHECK_STR( buf, "22222222\n" );
    write_file( IOS_S17_SERIAL_FILE, "garbage", 7 );
    fixed_next = 0; fixed_values[0] = 0x33333333;
    CHECK_EQ( ios_s17_load_serial( dir, fixed_random ), 0x33333333 );
    CHECK_EQ( read_file( IOS_S17_SERIAL_FILE, buf, sizeof(buf) ), 9 );
    CHECK_STR( buf, "33333333\n" );

    /* no file and an unwritable directory: a value for this run, no file */
    remove_file( IOS_S17_SERIAL_FILE );
    {
        char sub[512];
        snprintf( sub, sizeof(sub), "%s/missing-dir", dir );
        fixed_next = 0; fixed_values[0] = 0x44444444;
        CHECK_EQ( ios_s17_load_serial( sub, fixed_random ), 0x44444444 );
    }
}

/* the O_EXCL race: a second creator that finds the file takes the first one's value */
static int race_hook_armed;

static uint32_t racing_random(void)
{
    /* while we "draw", another program creates the file first */
    if (race_hook_armed)
    {
        race_hook_armed = 0;
        write_file( IOS_S17_SERIAL_FILE, "0abcdef0\n", 9 );
    }
    return 0x55555555;
}

struct race_arg { uint32_t result; };

static uint32_t thread_random(void)
{
    return (uint32_t)(uintptr_t)pthread_self() | 1;
}

static void *race_thread( void *arg )
{
    struct race_arg *r = arg;
    r->result = ios_s17_load_serial( dir, thread_random );
    return NULL;
}

static void test_serial_race(void)
{
    struct race_arg args[8];
    pthread_t threads[8];
    char buf[64];
    int i, round, same = 1;

    remove_file( IOS_S17_SERIAL_FILE );
    race_hook_armed = 1;
    CHECK_EQ( ios_s17_load_serial( dir, racing_random ), 0x0abcdef0 );
    CHECK_EQ( read_file( IOS_S17_SERIAL_FILE, buf, sizeof(buf) ), 9 );
    CHECK_STR( buf, "0abcdef0\n" );

    /* eight threads creating at once all end with the file's value; 200 rounds, from a missing
     * file and from a file holding 0 */
    for (round = 0; round < 200; round++)
    {
        if (round & 1) write_file( IOS_S17_SERIAL_FILE, "00000000\n", 9 );
        else remove_file( IOS_S17_SERIAL_FILE );
        for (i = 0; i < 8; i++) pthread_create( &threads[i], NULL, race_thread, &args[i] );
        for (i = 0; i < 8; i++) pthread_join( threads[i], NULL );
        if (read_file( IOS_S17_SERIAL_FILE, buf, sizeof(buf) ) != 9) same = 0;
        for (i = 0; i < 8; i++)
            if (args[i].result != ios_s17_parse_serial( buf, 9 )) same = 0;
        if (!same) { printf( "race round %d: values differ\n", round ); break; }
    }
    CHECK( same );
    /* no temporary file is left behind */
    {
        char tmp[600];
        snprintf( tmp, sizeof(tmp), "%s/%s.%ld.tmp", dir, IOS_S17_SERIAL_FILE, (long)getpid() );
        CHECK( access( tmp, F_OK ) != 0 );
    }
}

static void test_volume_info(void)
{
    union { FILE_FS_VOLUME_INFORMATION info; char bytes[256]; } u;
    ULONG_PTR information = 0x1234;

    memset( &u, 0xcc, sizeof(u) );
    CHECK_EQ( ios_s17_volume_info( &u.info, 0, &information ), STATUS_NOT_IMPLEMENTED );
    CHECK_EQ( information, 0x1234 );   /* untouched: upstream's answer */

    CHECK_EQ( ios_s17_volume_info( &u.info, 0x1d5b1f12, &information ), STATUS_SUCCESS );
    CHECK_EQ( u.info.VolumeCreationTime.QuadPart, 0 );
    CHECK_EQ( u.info.VolumeSerialNumber, 0x1d5b1f12 );
    CHECK_EQ( u.info.VolumeLabelLength, 0 );
    CHECK_EQ( u.info.SupportsObjects, TRUE );
    CHECK_EQ( information, offsetof( FILE_FS_VOLUME_INFORMATION, VolumeLabel ) );
    CHECK_EQ( information, 18 );
}

static void test_wow_volume_serial(void)
{
    char buf[64];
    ULONG first;

    /* the cached value: computed from the prefix's file, then the same for the app run */
    write_file( IOS_S17_SERIAL_FILE, "12345678\n", 9 );
    first = ios_s17_wow_volume_serial( dir, 'c' );
    CHECK_EQ( first, 0x1d5b1f12 );
    CHECK_EQ( ios_s17_wow_volume_serial( dir, 0 ), 0xbc5a865f );
    write_file( IOS_S17_SERIAL_FILE, "deadbeef\n", 9 );   /* not re-read during the run */
    CHECK_EQ( ios_s17_wow_volume_serial( dir, 'c' ), first );
    CHECK_EQ( read_file( IOS_S17_SERIAL_FILE, buf, sizeof(buf) ), 9 );
}

/* ---------------------------------------------------------------- Part C2 */

static void test_failed_opens(void)
{
    struct ios_s17_budgets b = { 0, 0, 0 };
    UNICODE_STRING nt;
    WCHAR name[] = { '\\','?','?','\\','C',':','\\','s','a','v','e',0xe9,'.','d','a','t' };
    char out[32];
    int i, taken;

    /* classification: write access or a disposition other than FILE_OPEN */
    CHECK( !ios_s17_open_is_write( FILE_READ_DATA | SYNCHRONIZE, FILE_OPEN ) );
    CHECK( !ios_s17_open_is_write( GENERIC_READ, FILE_OPEN ) );
    CHECK( ios_s17_open_is_write( FILE_WRITE_DATA, FILE_OPEN ) );
    CHECK( ios_s17_open_is_write( FILE_APPEND_DATA, FILE_OPEN ) );
    CHECK( ios_s17_open_is_write( GENERIC_WRITE, FILE_OPEN ) );
    CHECK( ios_s17_open_is_write( GENERIC_READ, FILE_CREATE ) );
    CHECK( ios_s17_open_is_write( GENERIC_READ, FILE_OPEN_IF ) );
    CHECK( ios_s17_open_is_write( GENERIC_READ, FILE_OVERWRITE_IF ) );
    CHECK( ios_s17_open_is_write( GENERIC_READ, FILE_SUPERSEDE ) );
    CHECK( ios_s17_open_is_write( GENERIC_READ, FILE_OVERWRITE ) );

    /* two separate budgets */
    for (i = taken = 0; i < 100; i++) taken += ios_s17_budget_take( &b, 0 );
    CHECK_EQ( taken, 64 );
    for (i = taken = 0; i < 300; i++) taken += ios_s17_budget_take( &b, 1 );
    CHECK_EQ( taken, 192 );   /* not used up by the read failures before */
    b.any = b.write = 0;
    for (i = taken = 0; i < 300; i++) taken += ios_s17_budget_take( &b, 1 );
    CHECK_EQ( taken, 192 );
    CHECK( ios_s17_budget_take( &b, 0 ) );   /* and the other way round */

    /* printable ASCII */
    ios_s17_ascii_a( out, sizeof(out), "/pfx/drive_c/a\tb\xc3\xa9" );
    CHECK_STR( out, "/pfx/drive_c/a?b??" );
    ios_s17_ascii_w( out, sizeof(out), (const uint16_t *)name, ARRAY_SIZE(name) );
    CHECK_STR( out, "\\??\\C:\\save?.dat" );
    ios_s17_ascii_a( out, 5, "abcdefgh" );
    CHECK_STR( out, "abcd" );

    /* the log lines: share mode only for writes and creates; then the budget stops them */
    nt.Buffer = name; nt.Length = sizeof(name); nt.MaximumLength = sizeof(name);
    log_lines = 0;
    ios_s17_log_failed_open( "NtCreateFile", STATUS_SHARING_VIOLATION, FILE_OVERWRITE_IF, GENERIC_WRITE,
                             0x60, 1, "/pfx/drive_c/save.dat", &nt );
    CHECK_EQ( log_lines, 1 );
    CHECK( strncmp( last_log, "NtCreateFile failed: ", 21 ) == 0 );
    CHECK( strstr( last_log, "share 0x1" ) != NULL );
    CHECK( strstr( last_log, "status 0xc0000043" ) != NULL );
    CHECK( strstr( last_log, "nt \\??\\C:\\save?.dat" ) != NULL );
    ios_s17_log_failed_open( "NtCreateFile", STATUS_OBJECT_NAME_NOT_FOUND, FILE_OPEN, GENERIC_READ, 0, 7,
                             NULL, NULL );
    CHECK_EQ( log_lines, 2 );
    CHECK( strstr( last_log, "share" ) == NULL );
    CHECK( strstr( last_log, "unix (none) nt (none)" ) != NULL );
    for (i = 0; i < 200; i++)
        ios_s17_log_failed_open( "NtCreateFile", STATUS_OBJECT_NAME_NOT_FOUND, FILE_OPEN, GENERIC_READ, 0, 7,
                                 "/x", NULL );
    CHECK_EQ( log_lines, 64 + 1 );
    for (i = 0; i < 400; i++)
        ios_s17_log_failed_open( "NtCreateFile", STATUS_ACCESS_DENIED, FILE_CREATE, GENERIC_WRITE, 0, 0,
                                 "/y", NULL );
    CHECK_EQ( log_lines, 64 + 192 );
}

/* C2 addendum: NtOpenFile and the attribute queries; C:\windows\ misses can't starve the rest */
static void test_failed_lookups(void)
{
    static const WCHAR sys[] = { '\\','?','?','\\','C',':','\\','W','i','n','d','o','w','s','\\',
                                 's','y','s','t','e','m','3','2','\\','x','.','d','l','l' };
    static const WCHAR game[] = { '\\','?','?','\\','C',':','\\','G','a','m','e','\\',
                                  'd','a','t','a','.','p','c','k' };
    static const WCHAR plain[] = { 'c',':','\\','w','i','n','d','o','w','s' };
    static const WCHAR dos[] = { '\\','\\','?','\\','C',':','\\','W','I','N','D','O','W','S','/','a' };
    static const WCHAR lookalike[] = { '\\','?','?','\\','C',':','\\','w','i','n','d','o','w','s','x','\\','a' };
    static const WCHAR other_drive[] = { '\\','?','?','\\','D',':','\\','w','i','n','d','o','w','s','\\','a' };
    struct ios_s17_budgets b = { 0, 0, 0 };
    UNICODE_STRING nt_sys, nt_game;
    int i, taken;

    CHECK( ios_s17_is_windows_path( (const uint16_t *)sys, ARRAY_SIZE(sys) ) );
    CHECK( ios_s17_is_windows_path( (const uint16_t *)plain, ARRAY_SIZE(plain) ) );
    CHECK( ios_s17_is_windows_path( (const uint16_t *)dos, ARRAY_SIZE(dos) ) );
    CHECK( !ios_s17_is_windows_path( (const uint16_t *)game, ARRAY_SIZE(game) ) );
    CHECK( !ios_s17_is_windows_path( (const uint16_t *)lookalike, ARRAY_SIZE(lookalike) ) );
    CHECK( !ios_s17_is_windows_path( (const uint16_t *)other_drive, ARRAY_SIZE(other_drive) ) );
    CHECK( !ios_s17_is_windows_path( (const uint16_t *)plain, ARRAY_SIZE(plain) - 1 ) );
    CHECK( !ios_s17_is_windows_path( NULL, 0 ) );

    CHECK_EQ( ios_s17_budget_kind( 1, (const uint16_t *)sys, ARRAY_SIZE(sys) ), IOS_S17_BUDGET_WRITE );
    CHECK_EQ( ios_s17_budget_kind( 1, (const uint16_t *)game, ARRAY_SIZE(game) ), IOS_S17_BUDGET_WRITE );
    CHECK_EQ( ios_s17_budget_kind( 0, (const uint16_t *)sys, ARRAY_SIZE(sys) ), IOS_S17_BUDGET_ANY );
    CHECK_EQ( ios_s17_budget_kind( 0, NULL, 0 ), IOS_S17_BUDGET_ANY );
    CHECK_EQ( ios_s17_budget_kind( 0, (const uint16_t *)game, ARRAY_SIZE(game) ), IOS_S17_BUDGET_PROGRAM );

    for (i = taken = 0; i < 400; i++) taken += ios_s17_budget_take( &b, IOS_S17_BUDGET_PROGRAM );
    CHECK_EQ( taken, 256 );
    CHECK( ios_s17_budget_take( &b, IOS_S17_BUDGET_ANY ) );
    CHECK( ios_s17_budget_take( &b, IOS_S17_BUDGET_WRITE ) );

    /* test_failed_opens used up the shared and write budgets: C:\windows\ misses stay quiet,
     * a program's own NtOpenFile and attribute-query failures still show, under their names */
    nt_sys.Buffer = (WCHAR *)sys; nt_sys.Length = nt_sys.MaximumLength = sizeof(sys);
    nt_game.Buffer = (WCHAR *)game; nt_game.Length = nt_game.MaximumLength = sizeof(game);
    log_lines = 0;
    ios_s17_log_failed_open( "NtOpenFile", STATUS_OBJECT_NAME_NOT_FOUND, FILE_OPEN, GENERIC_READ, 0, 7,
                             "/pfx/drive_c/windows/system32/x.dll", &nt_sys );
    ios_s17_log_failed_query( "NtQueryAttributesFile", STATUS_OBJECT_NAME_NOT_FOUND, NULL, &nt_sys );
    CHECK_EQ( log_lines, 0 );
    ios_s17_log_failed_open( "NtOpenFile", STATUS_OBJECT_PATH_NOT_FOUND, FILE_OPEN, GENERIC_READ, 0x20, 7,
                             "/pfx/drive_c/Game/data.pck", &nt_game );
    CHECK_EQ( log_lines, 1 );
    CHECK_STR( last_log, "NtOpenFile failed: status 0xc000003a disposition 1 access 0x80000000 options 0x20 "
                         "unix /pfx/drive_c/Game/data.pck nt \\??\\C:\\Game\\data.pck\n" );
    ios_s17_log_failed_query( "NtQueryAttributesFile", STATUS_OBJECT_NAME_NOT_FOUND, NULL, &nt_game );
    CHECK_EQ( log_lines, 2 );
    CHECK_STR( last_log, "NtQueryAttributesFile failed: status 0xc0000034 unix (none) nt \\??\\C:\\Game\\data.pck\n" );
    ios_s17_log_failed_query( "NtQueryFullAttributesFile", STATUS_OBJECT_NAME_NOT_FOUND,
                              "/pfx/drive_c/Game/data.pck", &nt_game );
    CHECK_EQ( log_lines, 3 );
    CHECK( strncmp( last_log, "NtQueryFullAttributesFile failed: status 0xc0000034 unix /pfx", 61 ) == 0 );
    for (i = 0; i < 400; i++)
        ios_s17_log_failed_query( "NtQueryAttributesFile", STATUS_OBJECT_NAME_NOT_FOUND, NULL, &nt_game );
    CHECK_EQ( log_lines, 256 );
}

/* ---------------------------------------------------------------- Part D3 */

static void test_cpu_count(void)
{
    static const char cfg[] =
        "\xef\xbb\xbf# settings\n"
        "cpu-count = 2\n"
        "  env.PG_X = a=b  \n"
        "no equals here\n"
        "  # cpu-count = 9\n"
        "cpu-count\t=  4 \r\n";
    char value[32];
    long n;

    /* the settings file reader */
    CHECK( ios_s17_cfg_lookup( cfg, sizeof(cfg) - 1, "cpu-count", value, sizeof(value) ) );
    CHECK_STR( value, "4" );   /* the last occurrence wins; # lines don't count */
    CHECK( ios_s17_cfg_lookup( cfg, sizeof(cfg) - 1, "env.PG_X", value, sizeof(value) ) );
    CHECK_STR( value, "a=b" );
    CHECK( !ios_s17_cfg_lookup( cfg, sizeof(cfg) - 1, "cpu", value, sizeof(value) ) );
    CHECK( !ios_s17_cfg_lookup( cfg, sizeof(cfg) - 1, "no equals here", value, sizeof(value) ) );

    CHECK( ios_s17_parse_int( " 12 ", &n ) && n == 12 );
    CHECK( ios_s17_parse_int( "-3", &n ) && n == -3 );
    CHECK( !ios_s17_parse_int( "4x", &n ) );
    CHECK( !ios_s17_parse_int( "", &n ) );
    CHECK( !ios_s17_parse_int( NULL, &n ) );

    /* the variable wins over the key */
    CHECK_EQ( ios_s17_cpu_override( "2", "4" ), 2 );
    CHECK_EQ( ios_s17_cpu_override( NULL, "4" ), 4 );
    CHECK_EQ( ios_s17_cpu_override( "", "4" ), 4 );
    /* 0, negative or garbage in the variable: not a positive integer, the key decides */
    CHECK_EQ( ios_s17_cpu_override( "0", "4" ), 4 );
    CHECK_EQ( ios_s17_cpu_override( "-3", "4" ), 4 );
    CHECK_EQ( ios_s17_cpu_override( "lots", "4" ), 4 );
    CHECK_EQ( ios_s17_cpu_override( "lots", NULL ), 0 );
    /* key 0 or absent: none */
    CHECK_EQ( ios_s17_cpu_override( NULL, "0" ), 0 );
    CHECK_EQ( ios_s17_cpu_override( NULL, NULL ), 0 );
    CHECK_EQ( ios_s17_cpu_override( NULL, "-2" ), 0 );
    CHECK_EQ( ios_s17_cpu_override( NULL, "x" ), 0 );
    /* only 1 to 63 is applied */
    CHECK_EQ( ios_s17_cpu_override( "64", "4" ), 0 );
    CHECK_EQ( ios_s17_cpu_override( "63", NULL ), 63 );
    CHECK_EQ( ios_s17_cpu_override( "1", NULL ), 1 );
    CHECK_EQ( ios_s17_cpu_override( NULL, "64" ), 0 );

    /* the file through PG_DOCS_DIR, decided once and logged once */
    write_file( "pocketgames.cfg", "cpu-count = 4\n", 14 );
    CHECK( ios_s17_cfg_file_get( dir, "cpu-count", value, sizeof(value) ) );
    CHECK_STR( value, "4" );
    CHECK( !ios_s17_cfg_file_get( "/nonexistent-dir", "cpu-count", value, sizeof(value) ) );
    CHECK( !ios_s17_cfg_file_get( NULL, "cpu-count", value, sizeof(value) ) );
    setenv( "PG_DOCS_DIR", dir, 1 );
    setenv( "PG_CPU_COUNT", "2", 1 );
    log_lines = 0;
    CHECK_EQ( ios_s17_cpu_count( 6 ), 2 );
    CHECK_EQ( log_lines, 1 );
    CHECK( strstr( last_log, "reporting 2 (host has 6)" ) != NULL );
    unsetenv( "PG_CPU_COUNT" );          /* later pseudo-processes keep the decision */
    CHECK_EQ( ios_s17_cpu_count( 6 ), 2 );
    CHECK_EQ( log_lines, 1 );
}

/* ---------------------------------------------------------------- Part D4 */

/* Clarification 2: fake pseudo-processes (ios_process.c's getter keeps the table in struct
 * ios_proc the same way), and a model of the PE side's RtlWaitOnAddress queues (spec 16's
 * sync.c): a queue head is a list head plus a lock, zero = empty, initialised on first use; the
 * queue of an address is (address >> 4) % 256; a wake walks the queue's entries. */
struct fake_proc { void *futex_queues; };
static struct fake_proc *fake_current;

void *ios_get_process_futex_queues(void)
{
    return fake_current ? ios_s17_futex_table_get( &fake_current->futex_queues ) : NULL;
}

struct model_list { struct model_list *next, *prev; };
struct model_queue { struct model_list queue; LONG lock; };
struct model_waiter { struct model_list entry; uintptr_t addr; };

static struct model_queue *model_queue_of( void *table, uintptr_t addr )
{
    return (struct model_queue *)table + ((addr >> 4) % IOS_S17_FUTEX_QUEUES);
}

static void model_wait( void *table, struct model_waiter *waiter, uintptr_t addr )
{
    struct model_queue *q = model_queue_of( table, addr );

    if (!q->queue.next) q->queue.next = q->queue.prev = &q->queue;
    waiter->addr = addr;
    waiter->entry.next = &q->queue;
    waiter->entry.prev = q->queue.prev;
    q->queue.prev->next = &waiter->entry;
    q->queue.prev = &waiter->entry;
}

static int model_wake_all( void *table, uintptr_t addr )
{
    struct model_queue *q = model_queue_of( table, addr );
    struct model_list *e;
    int found = 0;

    if (!q->queue.next) return 0;   /* never used: empty */
    for (e = q->queue.next; e != &q->queue; e = e->next)
        if (((struct model_waiter *)e)->addr == addr) found++;
    return found;
}

static void *query_table( NTSTATUS *status )
{
    void *out = NULL;
    ULONG len = 0;

    *status = 0x1234;
    if (!ios_s17_query_futex_queues( &out, sizeof(void *), &len, status )) return (void *)1;
    if (*status == STATUS_SUCCESS && len != sizeof(void *)) return (void *)2;
    return out;
}

static void *two_images_thread( void *arg )
{
    NTSTATUS status;
    *(void **)arg = query_table( &status );
    return NULL;
}

static void test_futex_queues(void)
{
    struct fake_proc proc1 = { NULL }, proc2 = { NULL }, proc3 = { NULL };
    struct model_waiter parked, *stale;
    void *table1, *table2, *out = NULL, *seen[8];
    pthread_t threads[8];
    ULONG len = 0;
    NTSTATUS status = 0x1234;
    const unsigned char *bytes;
    size_t i;
    int zero = 1, found = 0, same = 1;

    CHECK_EQ( IOS_S17_FUTEX_INFO_CLASS, 0xf00d );

    /* test 1: program 1 leaves a waiter linked in queue k and ends ("suspended for good") */
    fake_current = &proc1;
    table1 = query_table( &status );
    CHECK_EQ( status, STATUS_SUCCESS );
    CHECK( table1 != NULL );
    bytes = table1;
    for (i = 0; i < IOS_S17_FUTEX_QUEUES * 24; i++) if (bytes[i]) zero = 0;
    CHECK( zero );   /* created zero-filled */
    model_wait( table1, &parked, 0x7a3c40 );   /* queue (0x7a3c40 >> 4) % 256 = 0xc4 */
    CHECK_EQ( model_wake_all( table1, 0x7a3c40 ), 1 );
    /* the parked thread's stack is reused for something else: its entry's next is garbage */
    stale = &parked;
    stale->entry.next = (struct model_list *)(uintptr_t)0x10;

    /* program 2 in the same session: a different table, all zero */
    fake_current = &proc2;
    table2 = query_table( &status );
    CHECK_EQ( status, STATUS_SUCCESS );
    CHECK( table2 != NULL && table2 != table1 );
    bytes = table2;
    zero = 1;
    for (i = 0; i < IOS_S17_FUTEX_QUEUES * 24; i++) if (bytes[i]) zero = 0;
    CHECK( zero );

    /* test 3: a wake over addresses covering every queue index (bits 4-11) finds nothing and
     * never reaches program 1's stale entry (walking it would fault on next = 0x10) */
    for (i = 0; i < IOS_S17_FUTEX_QUEUES; i++)
    {
        found += model_wake_all( table2, 0x7a3000 + (i << 4) );
        found += model_wake_all( table2, 0x140000000ull + (i << 4) + 8 );
    }
    CHECK_EQ( found, 0 );
    CHECK( model_queue_of( table2, 0x7a3c40 ) != model_queue_of( table1, 0x7a3c40 ) );

    /* test 2: two images of one pseudo-process get the same table, also when they ask at once */
    CHECK( query_table( &status ) == table2 );
    CHECK_EQ( status, STATUS_SUCCESS );
    fake_current = &proc3;
    for (i = 0; i < 8; i++) pthread_create( &threads[i], NULL, two_images_thread, &seen[i] );
    for (i = 0; i < 8; i++) pthread_join( threads[i], NULL );
    for (i = 0; i < 8; i++) if (seen[i] != seen[0] || !seen[0]) same = 0;
    CHECK( same );
    CHECK( seen[0] != table1 && seen[0] != table2 );
    CHECK( proc3.futex_queues == seen[0] );

    /* test 4: the argument checks answer as before */
    fake_current = &proc2;
    CHECK( ios_s17_query_futex_queues( &out, sizeof(void *), NULL, &status ) );   /* no length asked */
    CHECK_EQ( status, STATUS_SUCCESS );
    CHECK( out == table2 );
    CHECK( ios_s17_query_futex_queues( NULL, sizeof(void *), &len, &status ) );
    CHECK_EQ( status, STATUS_ACCESS_VIOLATION );
    status = 0x1234;
    CHECK( !ios_s17_query_futex_queues( &out, 4, &len, &status ) );   /* other sizes: upstream's */
    CHECK( !ios_s17_query_futex_queues( &out, 16, &len, &status ) );
    CHECK( !ios_s17_query_futex_queues( NULL, 0, &len, &status ) );
    CHECK_EQ( status, 0x1234 );

    /* the fallback: a thread with no pseudo-process gets no table (its image keeps its own) */
    fake_current = NULL;
    out = NULL;
    CHECK( ios_s17_query_futex_queues( &out, sizeof(void *), &len, &status ) );
    CHECK_EQ( status, STATUS_UNSUCCESSFUL );
    CHECK( out == NULL );

    /* freed with its process; freeing twice is harmless */
    ios_s17_futex_table_free( &proc2.futex_queues );
    CHECK( proc2.futex_queues == NULL );
    ios_s17_futex_table_free( &proc2.futex_queues );
    ios_s17_futex_table_free( &proc3.futex_queues );
    /* program 1's table is kept here, as its struct is while a thread may still run */
    ios_s17_futex_table_free( &proc1.futex_queues );
}

/* ---------------------------------------------------------------- Part D5 */

static void test_syscall_name(void)
{
    static const char *const names[] = { "NtA", "NtB", "NtC" };

    CHECK( ios_s17_syscall_name( NULL, 3, 0 ) == NULL );
    CHECK_STR( ios_s17_syscall_name( names, 3, 0 ), "NtA" );
    CHECK_STR( ios_s17_syscall_name( names, 3, 2 ), "NtC" );
    CHECK( ios_s17_syscall_name( names, 3, 3 ) == NULL );
    CHECK( ios_s17_syscall_name( names, 0, 0 ) == NULL );   /* no service limit registered */
    CHECK( ios_s17_syscall_name( names, 3, 0xfff ) == NULL );
}

/* ---------------------------------------------------------------- Part D6 */

static int callback_calls;
static void alias_callback( unsigned long long a, unsigned long long b, unsigned long long c )
{
    callback_calls++;
}

static void test_unix_calls(void)
{
    struct ios_push_jit_aliases_params push = { alias_callback };
    struct ios_register_hold_release_params hold;
    struct ios_jit_alias_probe_params probe;
    struct ios_get_fex_arena_params arena = { 5, 6 };
    ULONG64 bridge = 77;
    int dummy;

    /* sizes and numbers */
    CHECK_EQ( sizeof(struct ios_register_hold_release_params), 24 );
    CHECK_EQ( sizeof(struct ios_jit_alias_probe_params), 88 );
    CHECK_EQ( sizeof(struct ios_get_fex_arena_params), 16 );
    CHECK_EQ( unix_system_time_precise, 7 );
    CHECK_EQ( unix_ios_push_jit_aliases, 8 );
    CHECK_EQ( unix_ios_register_hold_release, 9 );
    CHECK_EQ( unix_ios_jit_alias_probe, 10 );
    CHECK_EQ( unix_ios_mono_bridge_ptr, 11 );
    CHECK_EQ( unix_ios_get_fex_arena, 12 );

    /* 8 */
    CHECK_EQ( ios_unixcall_push_jit_aliases( NULL ), STATUS_INVALID_PARAMETER );
    push.callback = NULL;
    CHECK_EQ( ios_unixcall_push_jit_aliases( &push ), STATUS_INVALID_PARAMETER );
    push.callback = alias_callback;
    CHECK_EQ( ios_unixcall_push_jit_aliases( &push ), STATUS_SUCCESS );
    CHECK_EQ( callback_calls, 0 );

    /* 9 */
    hold.size = sizeof(hold); hold.version = 1; hold.peb = &dummy; hold.callback = &dummy;
    CHECK_EQ( ios_unixcall_register_hold_release( &hold ), STATUS_SUCCESS );
    CHECK_EQ( ios_unixcall_register_hold_release( NULL ), STATUS_INVALID_PARAMETER );
    hold.size = 20;
    CHECK_EQ( ios_unixcall_register_hold_release( &hold ), STATUS_INVALID_PARAMETER );
    hold.size = sizeof(hold); hold.version = 2;
    CHECK_EQ( ios_unixcall_register_hold_release( &hold ), STATUS_INVALID_PARAMETER );
    hold.version = 1; hold.peb = NULL;
    CHECK_EQ( ios_unixcall_register_hold_release( &hold ), STATUS_INVALID_PARAMETER );
    hold.peb = &dummy; hold.callback = NULL;
    CHECK_EQ( ios_unixcall_register_hold_release( &hold ), STATUS_INVALID_PARAMETER );

    /* 10 */
    memset( &probe, 0xab, sizeof(probe) );
    probe.size = sizeof(probe); probe.version = 1; probe.addr = 0x140001000;
    CHECK_EQ( ios_unixcall_jit_alias_probe( &probe ), STATUS_SUCCESS );
    CHECK_EQ( probe.size, 88 );
    CHECK_EQ( probe.version, 1 );
    CHECK_EQ( probe.addr, 0x140001000 );
    CHECK( !probe.rw && !probe.base && !probe.end && !probe.write_gen && !probe.written &&
           !probe.highest && !probe.at_end && !probe.rw_base && !probe.rx_base && !probe.slot &&
           !probe.dup_end );
    memset( &probe, 0xab, sizeof(probe) );
    probe.size = 80; probe.version = 1;
    CHECK_EQ( ios_unixcall_jit_alias_probe( &probe ), STATUS_INVALID_PARAMETER );
    CHECK_EQ( probe.rw, 0xababababababababull );   /* untouched */
    probe.size = sizeof(probe); probe.version = 0;
    CHECK_EQ( ios_unixcall_jit_alias_probe( &probe ), STATUS_INVALID_PARAMETER );
    CHECK_EQ( ios_unixcall_jit_alias_probe( NULL ), STATUS_INVALID_PARAMETER );

    /* 11 */
    CHECK_EQ( ios_unixcall_mono_bridge_ptr( &bridge ), STATUS_NOT_SUPPORTED );
    CHECK_EQ( bridge, 0 );
    CHECK_EQ( ios_unixcall_mono_bridge_ptr( NULL ), STATUS_NOT_SUPPORTED );

    /* 12 */
    CHECK_EQ( ios_unixcall_get_fex_arena( &arena ), STATUS_SUCCESS );
    CHECK_EQ( arena.base, 0 );
    CHECK_EQ( arena.end, 0 );
    CHECK_EQ( ios_unixcall_get_fex_arena( NULL ), STATUS_INVALID_PARAMETER );

    /* the WoW64 table answers all five STATUS_NOT_SUPPORTED, whatever the block */
    CHECK_EQ( ios_unixcall_wow64_not_supported( NULL ), STATUS_NOT_SUPPORTED );
    CHECK_EQ( ios_unixcall_wow64_not_supported( &arena ), STATUS_NOT_SUPPORTED );
    CHECK_EQ( arena.base, 0 );
}

/* ---------------------------------------------------------------- switches through the env */

static void test_switch_env(void)
{
    setenv( "PG_WOW64_BY_TEB", "0", 1 );
    unsetenv( "PG_IO_STATUS_OWNER" );
    setenv( "PG_SECTION_PROCESS_LIMIT", "1", 1 );
    CHECK( !ios_s17_switch( IOS_S17_WOW64_BY_TEB ) );
    CHECK( ios_s17_switch( IOS_S17_IO_STATUS_OWNER ) );
    CHECK( ios_s17_switch( IOS_S17_SECTION_LIMIT ) );
    /* read once per app run */
    setenv( "PG_WOW64_BY_TEB", "1", 1 );
    CHECK( !ios_s17_switch( IOS_S17_WOW64_BY_TEB ) );
}

int main(void)
{
    test_part_b();
    test_switch_env();
    test_serial_hash();
    test_serial_substitution();
    test_serial_parse();
    test_serial_file();
    test_serial_race();
    test_volume_info();
    test_wow_volume_serial();
    test_failed_opens();
    test_failed_lookups();   /* after test_failed_opens: relies on its used-up budgets */
    test_cpu_count();
    test_futex_queues();
    test_syscall_name();
    test_unix_calls();
    {
        char cmd[600];
        snprintf( cmd, sizeof(cmd), "rm -rf '%s'", dir );
        if (system( cmd )) printf( "note: could not remove %s\n", dir );
    }
    return check_summary();
}
