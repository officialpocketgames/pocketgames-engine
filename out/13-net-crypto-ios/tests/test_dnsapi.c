// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part K: dnsapi's unix side end to end with a fake resolver (res_ninit / res_nquery /
// res_ndestroy faked below). Compiled by run-host-tests.sh against the Wine headers together
// with dnsapi/libresolv.c and common/ios_resolv.c (IOS_RESOLV_TEST); the ntdll functions it
// needs are stubbed here.
//
//   test_dnsapi        with no servers configured a query gives DNS_ERROR_RCODE_SERVER_FAILURE
//                      and the server list DNS_ERROR_NO_DNS_SERVERS; set_serverlist's NULL
//                      block arriving as B is NULL in the 32-bit table; a configured server
//                      appears in the list
//   test_dnsapi off    with MADEIRA_DNSAPI_UNIXLIB=0 every entry gives STATUS_NOT_SUPPORTED

#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winbase.h"
#include "winerror.h"
#include "windns.h"
#include "wine/debug.h"
#include "wine/unixlib.h"
#include "dnsapi.h"

#ifndef IOS_RESOLV_TEST
#define IOS_RESOLV_TEST
#endif
#define IOS_RESOLV_NO_MACROS
#include "ios_resolv.h"

/* ---- stubs for what ntdll's unix side provides ---- */
unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel ) { return 0; }
const char * __cdecl __wine_dbg_strdup( const char *str ) { return str; }
int __cdecl __wine_dbg_output( const char *str ) { return fputs( str, stderr ); }
int __cdecl __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                               const char *function ) { return 0; }
DWORD ntdll_umbstowcs( const char *src, DWORD srclen, WCHAR *dst, DWORD dstlen )
{
    DWORD i;
    for (i = 0; i < srclen && i < dstlen; i++) dst[i] = (unsigned char)src[i];
    return i;
}
static unsigned long fake_base;
unsigned long ios_wow_base( void ) { return fake_base; }
ULONG ios_wow_guest_addr( const void *host ) { return (ULONG)((ULONG_PTR)host - fake_base); }

extern const unixlib_entry_t dnsapi_unix_call_funcs[];
extern const unixlib_entry_t dnsapi_unix_call_wow64_funcs[];

static int failures;
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
    if (a_ != b_) { failures++; printf( "FAIL %s:%d: %s == %s (%#llx != %#llx)\n", \
        __FILE__, __LINE__, #a, #b, a_, b_ ); } } while (0)

/* ---- fakes for res_ninit / res_nquery / res_ndestroy: a resolver configured with
 * fake_servers IPv4 servers (192.0.2.53, ...) whose queries all fail with TRY_AGAIN ---- */
static int fake_servers;

int ios_test_res_ninit( res_state state )
{
    int i;

    state->options |= RES_INIT;
    state->nscount = fake_servers;
    for (i = 0; i < fake_servers; i++)
    {
        state->nsaddr_list[i].sin_family = AF_INET;
        state->nsaddr_list[i].sin_addr.s_addr = htonl( 0xc0000235 + i );
    }
    return 0;
}

int ios_test_res_nquery( res_state state, const char *dname, int klass, int type,
                         unsigned char *answer, int anslen )
{
    state->res_h_errno = TRY_AGAIN;
    return -1;
}

void ios_test_res_ndestroy( res_state state ) { }

static NTSTATUS one_server_status;
static DNS_ADDR_ARRAY *one_server_addrs;
static DWORD one_server_len;

/* a fresh thread gets a fresh state, initialised with one server */
static void *one_server_thread( void *arg )
{
    struct get_serverlist_params params = { 2 /* WS_AF_INET */, one_server_addrs, &one_server_len };

    one_server_status = dnsapi_unix_call_funcs[unix_get_serverlist]( &params );
    return NULL;
}

int main( int argc, char **argv )
{
    BYTE buf[512], addrs_buf[256];
    DWORD len;
    struct get_serverlist_params serverlist = { 2 /* WS_AF_INET */, (DNS_ADDR_ARRAY *)addrs_buf, &len };
    struct query_params query = { "example.com", DNS_TYPE_A, DNS_QUERY_STANDARD, buf, &len };
    IP4_ARRAY ip4 = { 1, { 0x08080808 } };
    int off = argc > 1 && !strcmp( argv[1], "off" );

    if (off) setenv( "MADEIRA_DNSAPI_UNIXLIB", "0", 1 );
    else unsetenv( "MADEIRA_DNSAPI_UNIXLIB" );

    if (off)
    {
        len = sizeof(addrs_buf);
        CHECK_EQ( dnsapi_unix_call_funcs[unix_get_serverlist]( &serverlist ), STATUS_NOT_SUPPORTED );
        CHECK_EQ( dnsapi_unix_call_funcs[unix_set_serverlist]( NULL ), STATUS_NOT_SUPPORTED );
        len = sizeof(buf);
        CHECK_EQ( dnsapi_unix_call_funcs[unix_query]( &query ), STATUS_NOT_SUPPORTED );
        CHECK_EQ( dnsapi_unix_call_wow64_funcs[unix_set_serverlist]( NULL ), STATUS_NOT_SUPPORTED );
        printf( "test_dnsapi off: %d failed\n", failures );
        return failures ? 1 : 0;
    }

    /* the server list: no servers */
    len = sizeof(addrs_buf);
    CHECK_EQ( dnsapi_unix_call_funcs[unix_get_serverlist]( &serverlist ), DNS_ERROR_NO_DNS_SERVERS );
    /* a query: TRY_AGAIN -> DNS_ERROR_RCODE_SERVER_FAILURE, no crash */
    len = sizeof(buf);
    CHECK_EQ( dnsapi_unix_call_funcs[unix_query]( &query ), DNS_ERROR_RCODE_SERVER_FAILURE );

    /* 32-bit table: set_serverlist's NULL block arriving as B is NULL (nothing set) */
    fake_base = 0x300000000ul;
    CHECK_EQ( dnsapi_unix_call_wow64_funcs[unix_set_serverlist]( (void *)fake_base ), ERROR_SUCCESS );
    fake_base = 0;
    len = sizeof(addrs_buf);
    CHECK_EQ( dnsapi_unix_call_funcs[unix_get_serverlist]( &serverlist ), DNS_ERROR_NO_DNS_SERVERS );

    /* a real IP4_ARRAY is used (upstream writes it into the thread's state) and a query still
     * fails cleanly */
    CHECK_EQ( dnsapi_unix_call_funcs[unix_set_serverlist]( &ip4 ), ERROR_SUCCESS );
    len = sizeof(addrs_buf);
    CHECK_EQ( dnsapi_unix_call_funcs[unix_get_serverlist]( &serverlist ), ERROR_SUCCESS );
    len = sizeof(buf);
    CHECK_EQ( dnsapi_unix_call_funcs[unix_query]( &query ), DNS_ERROR_RCODE_SERVER_FAILURE );

    /* a resolver configured with a server: the list holds its IPv4 address */
    {
        pthread_t thread;
        const unsigned char *sa;
        unsigned short family;
        unsigned int addr;

        fake_servers = 1;
        one_server_addrs = (DNS_ADDR_ARRAY *)addrs_buf;
        one_server_len = sizeof(addrs_buf);
        memset( addrs_buf, 0, sizeof(addrs_buf) );
        pthread_create( &thread, NULL, one_server_thread, NULL );
        pthread_join( thread, NULL );
        CHECK_EQ( one_server_status, ERROR_SUCCESS );
        CHECK_EQ( one_server_addrs->AddrCount, 1 );
        /* a Windows SOCKADDR_IN: family at 0, address at 4 */
        sa = (const unsigned char *)one_server_addrs->AddrArray[0].MaxSa;
        memcpy( &family, sa, sizeof(family) );
        memcpy( &addr, sa + 4, sizeof(addr) );
        CHECK_EQ( family, 2 /* WS_AF_INET */ );
        CHECK_EQ( addr, htonl( 0xc0000235 ) );
    }

    printf( "test_dnsapi: %d failed\n", failures );
    return failures ? 1 : 0;
}
