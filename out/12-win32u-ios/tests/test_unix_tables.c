// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarifications 8 and 9: host test of the stand-in unix call tables. Built once per
// library (-DTEST_OPENGL32, -DTEST_WINSPOOL, -DTEST_NETAPI32, -DTEST_KERBEROS, -DTEST_MSV1_0):
// <name>_ios.c is compiled here against Wine 11.4's real header for that library
// (run-host-tests.sh runs gen-entries.sh on it first, with tables.txt's arguments);
// w32ios_unix_unsupported is a fake that records the report and index. Checks, per library: both
// tables have the enum's count of entries and no NULL; the setup entries (listed here, apart from
// tables.txt) return 0 and don't report; every other entry returns STATUS_NOT_SUPPORTED with its
// own index and its library's report; no entry touches its parameter block (a guard page: a touch
// ends the test with SIGSEGV); the names agree with spec 09's ios_unixlib_symbol. Also the "once
// per process" rule of unix_tables_pure.c.

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define END ~0u   /* ends the index lists below */

#if defined(TEST_OPENGL32)
#include "opengl32_ios.c"
#define DLL_FILE  "opengl32.dll"
#define FUNCS     opengl32_unix_call_funcs
#define WOW64     opengl32_unix_call_wow64_funcs
#define COUNT     funcs_count
#define HEAD      "[opengl32] no OpenGL on iOS"
#define FUNCS_NAME "opengl32_unix_call_funcs"
#define WOW64_NAME "opengl32_unix_call_wow64_funcs"
static const unsigned int setup[] = { unix_process_attach, unix_thread_attach, unix_process_detach, END };
static const unsigned int samples[] = { unix_wglCreateContext, unix_get_pixel_formats, unix_wglSwapIntervalEXT, END };

#elif defined(TEST_WINSPOOL)
#include "winspool_ios.c"
#define DLL_FILE  "winspool.drv"
#define FUNCS     winspool_unix_call_funcs
#define WOW64     winspool_unix_call_wow64_funcs
#define COUNT     unix_funcs_count
#define HEAD      "[winspool] no printing on iOS"
#define FUNCS_NAME "winspool_unix_call_funcs"
#define WOW64_NAME "winspool_unix_call_wow64_funcs"
static const unsigned int setup[] = { unix_process_attach, END };
static const unsigned int samples[] = { unix_enum_printers, unix_get_default_page_size, unix_get_ppd, END };

#elif defined(TEST_NETAPI32)
#include "netapi32_ios.c"
#define DLL_FILE  "netapi32.dll"
#define FUNCS     netapi32_unix_call_funcs
#define WOW64     netapi32_unix_call_wow64_funcs
#define COUNT     unix_funcs_count
#define HEAD      "[netapi32] no Samba on iOS"
#define FUNCS_NAME "netapi32_unix_call_funcs"
#define WOW64_NAME "netapi32_unix_call_wow64_funcs"
static const unsigned int setup[] = { unix_netapi_init, END };
static const unsigned int samples[] = { unix_server_getinfo, unix_share_add, unix_change_password, END };

#elif defined(TEST_KERBEROS)
#include "kerberos_ios.c"
#define DLL_FILE  "kerberos.dll"
#define FUNCS     kerberos_unix_call_funcs
#define WOW64     kerberos_unix_call_wow64_funcs
#define COUNT     unix_funcs_count
#define HEAD      "[kerberos] no Kerberos on iOS"
#define FUNCS_NAME "kerberos_unix_call_funcs"
#define WOW64_NAME "kerberos_unix_call_wow64_funcs"
static const unsigned int setup[] = { unix_process_attach, END };
static const unsigned int samples[] = { unix_acquire_credentials_handle, unix_query_ticket_cache, unix_verify_signature, END };

#elif defined(TEST_MSV1_0)
#include "msv1_0_ios.c"
#define DLL_FILE  "msv1_0.dll"
#define FUNCS     msv1_0_unix_call_funcs
#define WOW64     msv1_0_unix_call_wow64_funcs
#define COUNT     unix_funcs_count
#define HEAD      "[msv1_0] no NTLM on iOS"
#define FUNCS_NAME "msv1_0_unix_call_funcs"
#define WOW64_NAME "msv1_0_unix_call_wow64_funcs"
static const unsigned int setup[] = { END };   /* msv1_0 has no setup entry */
static const unsigned int samples[] = { unix_chat, unix_cleanup, unix_fork, END };

#else
#error "build with one of -DTEST_OPENGL32, -DTEST_WINSPOOL, -DTEST_NETAPI32, -DTEST_KERBEROS, -DTEST_MSV1_0"
#endif

#include "unix_tables_pure.h"

int ios_unixlib_symbol( const char *dll_name, int wow64, char *buf, size_t size );   /* spec 09 */

static int checks, failures;

#define CHECK( cond ) do { checks++; if (!(cond)) { failures++; \
    printf( "%s:%d: %s: CHECK failed: %s\n", __FILE__, __LINE__, DLL_FILE, #cond ); } } while (0)

static unsigned int fake_calls, fake_index;
static const struct w32ios_unix_report *fake_report;

NTSTATUS w32ios_unix_unsupported( const struct w32ios_unix_report *report, unsigned int index )
{
    fake_calls++;
    fake_index = index;
    fake_report = report;
    return STATUS_NOT_SUPPORTED;
}

static int in_list( const unsigned int *list, unsigned int index )
{
    for (; *list != END; list++) if (*list == index) return 1;
    return 0;
}

static void test_table( const unixlib_entry_t *table, size_t size, void *guard )
{
    unsigned int i, n_setup = 0, wrong_status = 0, wrong_index = 0, missing = 0;

    CHECK( size == COUNT );
    for (i = 0; i < size; i++) if (!table[i]) missing++;
    CHECK( missing == 0 );
    if (missing) return;

    /* the setup entries, with the parameter pointer on the guard page and NULL */
    fake_calls = 0;
    for (i = 0; setup[i] != END; i++, n_setup++)
    {
        CHECK( table[setup[i]]( guard ) == STATUS_SUCCESS );
        CHECK( table[setup[i]]( NULL ) == STATUS_SUCCESS );
    }
    CHECK( fake_calls == 0 );

    /* every other entry, with its parameter pointer on the guard page */
    for (i = 0; i < size; i++)
    {
        if (in_list( setup, i )) continue;
        fake_index = END;
        fake_report = NULL;
        if (table[i]( guard ) != STATUS_NOT_SUPPORTED) wrong_status++;
        if (fake_index != i || !fake_report || strcmp( fake_report->head, HEAD )) wrong_index++;
    }
    CHECK( wrong_status == 0 );
    CHECK( wrong_index == 0 );
    CHECK( fake_calls == size - n_setup );

    /* a few by name */
    for (i = 0; samples[i] != END; i++)
    {
        CHECK( !in_list( setup, samples[i] ) );
        fake_index = END;
        CHECK( table[samples[i]]( guard ) == STATUS_NOT_SUPPORTED );
        CHECK( fake_index == samples[i] );
    }
}

static void test_tables( void )
{
    long page = sysconf( _SC_PAGESIZE );
    char *block = mmap( NULL, 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );

    CHECK( block != MAP_FAILED );
    if (block == MAP_FAILED) return;
    test_table( FUNCS, ARRAYSIZE(FUNCS), block + page / 2 );
    test_table( WOW64, ARRAYSIZE(WOW64), block + page );
    munmap( block, 2 * page );
}

static void test_names( void )
{
    char buf[64], upper[64], path[96];
    size_t i;

    CHECK( !ios_unixlib_symbol( DLL_FILE, 0, buf, sizeof(buf) ) );
    CHECK( !strcmp( buf, FUNCS_NAME ) );
    CHECK( !ios_unixlib_symbol( DLL_FILE, 1, buf, sizeof(buf) ) );
    CHECK( !strcmp( buf, WOW64_NAME ) );

    /* as the loader may see it: a full path, upper case */
    for (i = 0; DLL_FILE[i] && i < sizeof(upper) - 1; i++)
        upper[i] = (DLL_FILE[i] >= 'a' && DLL_FILE[i] <= 'z') ? DLL_FILE[i] - 'a' + 'A' : DLL_FILE[i];
    upper[i] = 0;
    snprintf( path, sizeof(path), "C:\\windows\\syswow64\\%s", upper );
    CHECK( !ios_unixlib_symbol( path, 1, buf, sizeof(buf) ) );
    CHECK( !strcmp( buf, WOW64_NAME ) );
}

static void test_once_per_process( void )
{
    static struct w32ios_unix_seen seen;
    static char pebs[W32IOS_UNIX_SEEN + 1];
    unsigned int i, logged;

    CHECK( w32ios_unix_first_report( &seen, &pebs[0], 0x20 ) == 1 );
    CHECK( w32ios_unix_first_report( &seen, &pebs[0], 0x20 ) == 0 );
    CHECK( w32ios_unix_first_report( &seen, &pebs[0], 0x24 ) == 1 );   /* same PEB, another process */
    CHECK( w32ios_unix_first_report( &seen, &pebs[1], 0x20 ) == 1 );   /* same id, another PEB */
    CHECK( w32ios_unix_first_report( &seen, &pebs[1], 0x20 ) == 0 );
    CHECK( seen.count == 3 );

    /* full: the oldest is forgotten, the others stay */
    for (i = 2, logged = 0; i < W32IOS_UNIX_SEEN + 1; i++)
        logged += w32ios_unix_first_report( &seen, &pebs[i], 0x100 + i );
    CHECK( logged == W32IOS_UNIX_SEEN - 1 );
    CHECK( seen.count == W32IOS_UNIX_SEEN );
    CHECK( w32ios_unix_first_report( &seen, &pebs[W32IOS_UNIX_SEEN], 0x100 + W32IOS_UNIX_SEEN ) == 0 );
    CHECK( w32ios_unix_first_report( &seen, &pebs[1], 0x20 ) == 0 );
    CHECK( w32ios_unix_first_report( &seen, &pebs[0], 0x20 ) == 1 );   /* was overwritten */
    CHECK( seen.count == W32IOS_UNIX_SEEN );
}

int main( void )
{
    test_tables();
    test_names();
    test_once_per_process();
    printf( "%s: %d checks, %d failures\n", DLL_FILE, checks, failures );
    return failures != 0;
}
