// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part K: nsi.dll's unix-call table (nsi/nsi_unix.c) end to end with fake providers in
// place of ndis.c, ip.c and nsi_tcp.c. Compiled by run-host-tests.sh against the Wine headers
// with ios_cfg_nsi.h, like the real file.
//
//   test_nsi_dispatch       unserved tables, the parameter checks, the cache through the table
//                           (hit, TCP never cached, failures never cached, the providers' own
//                           lookups never cached), the 32-bit entries with a fake window base
//   test_nsi_dispatch off   PG_NSI_NETWORK_TABLES=0: NDIS/IPv4/IPv6 not served, TCP still

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ifdef.h"
#define USE_WS_PREFIX
#include "netiodef.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/unixlib.h"
#include "unix_private.h"
#include "ios_nsi_pure.h"

/* ---- stubs for what ntdll's unix side provides ---- */
unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel ) { return 0; }
const char * __cdecl __wine_dbg_strdup( const char *str ) { return str; }
int __cdecl __wine_dbg_output( const char *str ) { return fputs( str, stderr ); }
int __cdecl __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                               const char *function ) { return 0; }
static unsigned long fake_base;
unsigned long ios_wow_base( void ) { return fake_base; }
ULONG ios_wow_guest_addr( const void *host ) { return (ULONG)((ULONG_PTR)host - fake_base); }

extern const unixlib_entry_t nsi_unix_call_funcs[];
extern const unixlib_entry_t nsi_unix_call_wow64_funcs[];

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c ); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
    if (a_ != b_) { failures++; printf( "FAIL %s:%d: %s == %s (%#llx != %#llx)\n", \
        __FILE__, __LINE__, #a, #b, a_, b_ ); } } while (0)

/* ---- fake providers: 3 rows of key 8 / static 16 bytes; calls counted ---- */
#define ROWS 3
static int ndis_calls, tcp_calls;
static NTSTATUS fail_next;

static NTSTATUS fake_enumerate( int *calls, void *key, UINT key_size, void *rw, UINT rw_size,
                                void *dyn, UINT dyn_size, void *stat, UINT stat_size, UINT_PTR *count )
{
    BOOL want = key_size || rw_size || dyn_size || stat_size;
    UINT i;

    (*calls)++;
    if (fail_next) { NTSTATUS s = fail_next; fail_next = 0; return s; }
    for (i = 0; i < ROWS && i < *count; i++)
    {
        if (key) memset( (char *)key + i * key_size, 0x10 + i, key_size );
        if (stat) memset( (char *)stat + i * stat_size, 0x20 + i, stat_size );
    }
    if (!want || ROWS <= *count) { *count = ROWS; return STATUS_SUCCESS; }
    return STATUS_BUFFER_OVERFLOW;
}

static NTSTATUS ndis_enumerate( void *k, UINT ks, void *r, UINT rs, void *d, UINT ds, void *s, UINT ss, UINT_PTR *c )
{
    return fake_enumerate( &ndis_calls, k, ks, r, rs, d, ds, s, ss, c );
}

static NTSTATUS tcp_enumerate( void *k, UINT ks, void *r, UINT rs, void *d, UINT ds, void *s, UINT ss, UINT_PTR *c )
{
    return fake_enumerate( &tcp_calls, k, ks, r, rs, d, ds, s, ss, c );
}

static NTSTATUS ndis_get_parameter( const void *key, UINT key_size, UINT param_type,
                                    void *data, UINT data_size, UINT data_offset )
{
    memset( data, 0x77, data_size );
    return STATUS_SUCCESS;
}

static const struct module_table ndis_tables[] =
{
    { NSI_NDIS_IFINFO_TABLE, { 8, 0, 0, 16 }, ndis_enumerate, NULL, ndis_get_parameter },
    { ~0u }
};
static const struct module_table ip_tables[] = { { ~0u } };
static const struct module_table tcp_tables[] =
{
    { NSI_TCP_ALL_TABLE, { 8, 0, 0, 16 }, tcp_enumerate },
    { ~0u }
};

const struct module ndis_module = { &NPI_MS_NDIS_MODULEID, ndis_tables };
const struct module ipv4_module = { &NPI_MS_IPV4_MODULEID, ip_tables };
const struct module ipv6_module = { &NPI_MS_IPV6_MODULEID, ip_tables };
const struct module tcp_module = { &NPI_MS_TCP_MODULEID, tcp_tables };

static NTSTATUS call_enumerate( const NPI_MODULEID *module, UINT table, void *key, UINT key_size,
                                void *stat, UINT stat_size, UINT_PTR *count )
{
    struct nsi_enumerate_all_ex params;

    memset( &params, 0, sizeof(params) );
    params.module = module;
    params.table = table;
    params.first_arg = 1;
    params.key_data = key;
    params.key_size = key_size;
    params.static_data = stat;
    params.static_size = stat_size;
    params.count = *count;
    NTSTATUS status = nsi_unix_call_funcs[0]( &params );
    *count = params.count;
    return status;
}

static void test_off( void )
{
    unsigned char key[8 * ROWS], stat[16 * ROWS];
    UINT_PTR count = ROWS;

    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, NULL, 0, NULL, 0, &count ),
              STATUS_NOT_SUPPORTED );
    CHECK_EQ( ndis_calls, 0 );
    count = ROWS;
    CHECK_EQ( call_enumerate( &NPI_MS_TCP_MODULEID, NSI_TCP_ALL_TABLE, key, 8, stat, 16, &count ),
              STATUS_SUCCESS );
    CHECK_EQ( count, ROWS );
}

static void test_on( void )
{
    unsigned char key[8 * ROWS], stat[16 * ROWS];
    struct nsi_get_parameter_ex gp;
    UINT_PTR count;
    UINT index_out;
    NET_LUID luid = { 0 };

    /* unserved: unknown table, UDP module, NULL module */
    count = 0;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, 99, NULL, 0, NULL, 0, &count ), STATUS_NOT_SUPPORTED );
    CHECK_EQ( call_enumerate( &NPI_MS_UDP_MODULEID, 1, NULL, 0, NULL, 0, &count ), STATUS_NOT_SUPPORTED );
    CHECK_EQ( call_enumerate( NULL, 0, NULL, 0, NULL, 0, &count ), STATUS_NOT_SUPPORTED );
    /* a table without that operation (no get_all_parameters on the fake NDIS table) */
    {
        struct nsi_get_all_parameters_ex ga;
        memset( &ga, 0, sizeof(ga) );
        ga.module = &NPI_MS_NDIS_MODULEID;
        ga.table = NSI_NDIS_IFINFO_TABLE;
        ga.key = &luid;
        ga.key_size = 8;
        CHECK_EQ( nsi_unix_call_funcs[1]( &ga ), STATUS_NOT_SUPPORTED );
    }

    /* checks: wrong size, NULL buffer with a size */
    count = ROWS;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 7, NULL, 0, &count ),
              STATUS_INVALID_PARAMETER );
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, NULL, 8, NULL, 0, &count ),
              STATUS_INVALID_PARAMETER );
    CHECK_EQ( ndis_calls, 0 );

    /* the cache: the second identical enumerate doesn't reach the provider */
    count = ROWS;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 8, stat, 16, &count ), STATUS_SUCCESS );
    CHECK_EQ( ndis_calls, 1 );
    memset( key, 0, sizeof(key) );
    count = ROWS + 2;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 8, stat, 16, &count ), STATUS_SUCCESS );
    CHECK_EQ( ndis_calls, 1 );
    CHECK_EQ( count, ROWS );
    CHECK_EQ( key[8], 0x11 );
    /* smaller room from the cache: overflow, count untouched */
    count = 1;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 8, stat, 16, &count ),
              STATUS_BUFFER_OVERFLOW );
    CHECK_EQ( count, 1 );
    CHECK_EQ( ndis_calls, 1 );
    /* count only is a different request */
    count = 0;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, NULL, 0, NULL, 0, &count ), STATUS_SUCCESS );
    CHECK_EQ( count, ROWS );
    CHECK_EQ( ndis_calls, 2 );

    /* TCP is never cached */
    count = ROWS;
    CHECK_EQ( call_enumerate( &NPI_MS_TCP_MODULEID, NSI_TCP_ALL_TABLE, key, 8, stat, 16, &count ), STATUS_SUCCESS );
    count = ROWS;
    CHECK_EQ( call_enumerate( &NPI_MS_TCP_MODULEID, NSI_TCP_ALL_TABLE, key, 8, stat, 16, &count ), STATUS_SUCCESS );
    CHECK_EQ( tcp_calls, 2 );
    /* TCP: a rw size is invalid */
    {
        struct nsi_enumerate_all_ex p;
        memset( &p, 0, sizeof(p) );
        p.module = &NPI_MS_TCP_MODULEID;
        p.table = NSI_TCP_ALL_TABLE;
        p.rw_data = key;
        p.rw_size = 4;
        CHECK_EQ( nsi_unix_call_funcs[0]( &p ), STATUS_INVALID_PARAMETER );
    }

    /* failures are never cached: a failing request with a new shape, then the same again */
    fail_next = STATUS_UNSUCCESSFUL;
    count = ROWS;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 8, NULL, 0, &count ), STATUS_UNSUCCESSFUL );
    count = ROWS;
    CHECK_EQ( call_enumerate( &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 8, NULL, 0, &count ), STATUS_SUCCESS );
    CHECK_EQ( ndis_calls, 4 );

    /* the providers' own lookups go through the dispatcher but not the cache */
    {
        UINT before = ndis_calls, c = ROWS;
        CHECK( !nsi_enumerate_all( 1, 0, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 8, NULL, 0,
                                   NULL, 0, stat, 16, &c ) );
        CHECK( !nsi_enumerate_all( 1, 0, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, key, 8, NULL, 0,
                                   NULL, 0, stat, 16, &c ) );
        CHECK_EQ( ndis_calls, before + 2 );
        CHECK( convert_luid_to_index( &luid, &index_out ) );   /* get_parameter, static field */
        CHECK_EQ( index_out, 0x77777777 );
    }

    /* get_parameter checks: param_type, offset past the row */
    memset( &gp, 0, sizeof(gp) );
    gp.module = &NPI_MS_NDIS_MODULEID;
    gp.table = NSI_NDIS_IFINFO_TABLE;
    gp.key = &luid;
    gp.key_size = 8;
    gp.param_type = NSI_PARAM_TYPE_STATIC;
    gp.data = &index_out;
    gp.data_size = 4;
    gp.data_offset = 12;
    CHECK_EQ( nsi_unix_call_funcs[2]( &gp ), STATUS_SUCCESS );
    gp.data_offset = 13;
    CHECK_EQ( nsi_unix_call_funcs[2]( &gp ), STATUS_INVALID_PARAMETER );
    gp.data_offset = 0;
    gp.param_type = 3;
    CHECK_EQ( nsi_unix_call_funcs[2]( &gp ), STATUS_INVALID_PARAMETER );

    /* 32-bit entries: every pointer a guest address of a fake window, count written back */
    {
        unsigned char *arena = calloc( 1, 4096 );
        struct ios_nsi_enumerate_all_ex32 *p32 = (void *)arena;
        struct ios_nsi_get_parameter_ex32 *g32 = (void *)(arena + 128);
        NPI_MODULEID *mod = (void *)(arena + 256);
        unsigned char *k32 = arena + 512, *s32 = arena + 1024;

        fake_base = (uintptr_t)arena - 0x10000;
        *mod = NPI_MS_TCP_MODULEID;
        p32->module = 0x10000 + 256;
        p32->table = NSI_TCP_ALL_TABLE;
        p32->key_data = 0x10000 + 512;
        p32->key_size = 8;
        p32->static_data = 0x10000 + 1024;
        p32->static_size = 16;
        p32->count = ROWS;
        CHECK_EQ( nsi_unix_call_wow64_funcs[0]( p32 ), STATUS_SUCCESS );
        CHECK_EQ( p32->count, ROWS );
        CHECK_EQ( k32[0], 0x10 );
        CHECK_EQ( s32[16], 0x21 );
        p32->key_data = 0;            /* a guest NULL with a size */
        CHECK_EQ( nsi_unix_call_wow64_funcs[0]( p32 ), STATUS_INVALID_PARAMETER );

        *mod = NPI_MS_NDIS_MODULEID;
        *(NET_LUID *)k32 = luid;
        g32->module = 0x10000 + 256;
        g32->table = NSI_NDIS_IFINFO_TABLE;
        g32->key = 0x10000 + 512;
        g32->key_size = 8;
        g32->param_type = NSI_PARAM_TYPE_STATIC;
        g32->data = 0x10000 + 1024;
        g32->data_size = 4;
        g32->data_offset = 0;
        memset( s32, 0, 4 );
        CHECK_EQ( nsi_unix_call_wow64_funcs[2]( g32 ), STATUS_SUCCESS );
        CHECK_EQ( s32[0], 0x77 );
        fake_base = 0;
        free( arena );
    }
}

int main( int argc, char **argv )
{
    int off = argc > 1 && !strcmp( argv[1], "off" );

    setenv( "PG_NSI_CACHE_MS", "5000", 1 );   /* long enough that the test can't race it */
    if (off) setenv( "PG_NSI_NETWORK_TABLES", "0", 1 );
    else unsetenv( "PG_NSI_NETWORK_TABLES" );

    if (off) test_off();
    else test_on();

    printf( "test_nsi_dispatch%s: %d failed\n", off ? " off" : "", failures );
    return failures ? 1 : 0;
}
