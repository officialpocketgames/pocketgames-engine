// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part I: nsi.dll's own unix-call table (nsi_unix_call_funcs /
// nsi_unix_call_wow64_funcs). nsiproxy.sys is not shipped, so \\.\Nsi can't be opened; the
// shipped nsi.dll then calls these three entries:
//   0  struct nsi_enumerate_all_ex *       enumerate a table (count in/out)
//   1  struct nsi_get_all_parameters_ex *  one row
//   2  struct nsi_get_parameter_ex *       one field
// with the layouts of include/wine/nsi.h (32-bit callers: ios_nsi_pure.h, I1).
//
// The dispatcher serves the TCP connection tables (nsi_tcp.c, I2) and Wine's own BSD providers
// for interfaces (ndis.c), and IPv4/IPv6 addresses, routes and neighbours (upstream ip.c) (I3).
// Differences from upstream nsiproxy's dispatcher: a table not served gives
// STATUS_NOT_SUPPORTED (nsi.dll turns it back into the device-open error callers always got);
// a non-zero size with a NULL buffer, a size that isn't the table's, a param_type above 2 and a
// field read past the row give STATUS_INVALID_PARAMETER. The providers' own lookups of other
// tables (ios_nsi_enumerate_all_ex & co., unix_private.h) go through the same dispatcher.
// Enumerates from nsi.dll of the NDIS/IP tables are answered from a short cache (I4).
//
// State (Part J): the module list (const), the cache and the log counter (app-wide, under
// ios_nsi_lock / atomics), the two environment values (read once per app run).

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ifdef.h"
#define __WINE_INIT_NPI_MODULEID   /* the NPI_MS_*_MODULEID objects are defined here */
#define USE_WS_PREFIX
#include "netiodef.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unix_private.h"
#include "ios_guest.h"
#include "ios_nsi_pure.h"

C_ASSERT( IOS_NSI_STATUS_BUFFER_OVERFLOW == (UINT)STATUS_BUFFER_OVERFLOW );
C_ASSERT( IOS_NSI_STATUS_INVALID_PARAMETER == (UINT)STATUS_INVALID_PARAMETER );
C_ASSERT( IOS_NSI_STATUS_NOT_SUPPORTED == (UINT)STATUS_NOT_SUPPORTED );
C_ASSERT( IOS_NSI_STATUS_BUFFER_TOO_SMALL == (UINT)STATUS_BUFFER_TOO_SMALL );

enum ios_nsi_module { IOS_NSI_NDIS, IOS_NSI_IPV4, IOS_NSI_IPV6, IOS_NSI_TCP, IOS_NSI_MODULES };

static const struct module *const ios_nsi_modules[IOS_NSI_MODULES] =
{
    &ndis_module,
    &ipv4_module,
    &ipv6_module,
    &tcp_module,
};
static const char *const ios_nsi_module_names[IOS_NSI_MODULES] = { "ndis", "ipv4", "ipv6", "tcp" };

/* ---------------------------------------------------------------- environment, read once */

/* MADEIRA_NSI_NETWORK_TABLES=0 serves none of the NDIS, IPv4 and IPv6 modules */
static BOOL ios_nsi_network_tables(void)
{
    static int on = -1;
    int value = __atomic_load_n( &on, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = ios_nsi_env_on( getenv( "MADEIRA_NSI_NETWORK_TABLES" ));
        if (!value) fprintf( stderr, "[nsi-ios] MADEIRA_NSI_NETWORK_TABLES=0: interface, address and route tables off\n" );
        __atomic_store_n( &on, value, __ATOMIC_RELAXED );
    }
    return value;
}

/* MADEIRA_NSI_CACHE_MS: default 500, 0 off, capped at 5000 */
static UINT ios_nsi_cache_ms(void)
{
    static int ms = -1;
    int value = __atomic_load_n( &ms, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = ios_nsi_cache_ttl_from_env( getenv( "MADEIRA_NSI_CACHE_MS" ));
        __atomic_store_n( &ms, value, __ATOMIC_RELAXED );
    }
    return value;
}

static uint64_t ios_nsi_now_ms(void)
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- the dispatcher */

static const struct module_table *ios_nsi_find_table( const NPI_MODULEID *id, UINT_PTR table, int *index )
{
    const struct module_table *entry;
    int i;

    if (!id) return NULL;
    for (i = 0; i < IOS_NSI_MODULES; i++)
    {
        if (!NmrIsEqualNpiModuleId( ios_nsi_modules[i]->module, id )) continue;
        if (i != IOS_NSI_TCP && !ios_nsi_network_tables()) return NULL;
        for (entry = ios_nsi_modules[i]->tables; entry->table != ~0u; entry++)
        {
            if (entry->table != table) continue;
            if (index) *index = i;
            return entry;
        }
        return NULL;
    }
    return NULL;
}

/* checks shared by the providers' own lookups and nsi.dll's calls; *entry_ret and data_ret
 * receive the table and the buffers to pass (NULL for empty parts) */
static NTSTATUS ios_nsi_prepare_enumerate( const struct nsi_enumerate_all_ex *params,
                                           const struct module_table **entry_ret, int *index,
                                           UINT sizes[4], void *data_ret[4] )
{
    const struct module_table *entry = ios_nsi_find_table( params->module, params->table, index );
    void *data[4] = { params->key_data, params->rw_data, params->dynamic_data, params->static_data };

    if (!entry || !entry->enumerate_all) return STATUS_NOT_SUPPORTED;
    sizes[0] = params->key_size;
    sizes[1] = params->rw_size;
    sizes[2] = params->dynamic_size;
    sizes[3] = params->static_size;
    if (ios_nsi_check_enumerate( entry->sizes, sizes, data, data_ret )) return STATUS_INVALID_PARAMETER;
    *entry_ret = entry;
    return STATUS_SUCCESS;
}

NTSTATUS nsi_enumerate_all_ex( struct nsi_enumerate_all_ex *params )
{
    const struct module_table *entry;
    UINT sizes[4];
    void *data[4];
    int index;
    NTSTATUS status;

    if ((status = ios_nsi_prepare_enumerate( params, &entry, &index, sizes, data ))) return status;
    return entry->enumerate_all( data[0], sizes[0], data[1], sizes[1], data[2], sizes[2],
                                 data[3], sizes[3], &params->count );
}

NTSTATUS nsi_get_all_parameters_ex( struct nsi_get_all_parameters_ex *params )
{
    const struct module_table *entry = ios_nsi_find_table( params->module, params->table, NULL );
    UINT sizes[4] = { params->key_size, params->rw_size, params->dynamic_size, params->static_size };
    void *data[4] = { (void *)params->key, params->rw_data, params->dynamic_data, params->static_data };
    void *out[4];

    if (!entry || !entry->get_all_parameters) return STATUS_NOT_SUPPORTED;
    if (ios_nsi_check_get_all( entry->sizes, sizes, data, out )) return STATUS_INVALID_PARAMETER;
    return entry->get_all_parameters( params->key, params->key_size, out[1], params->rw_size,
                                      out[2], params->dynamic_size, out[3], params->static_size );
}

NTSTATUS nsi_get_parameter_ex( struct nsi_get_parameter_ex *params )
{
    const struct module_table *entry = ios_nsi_find_table( params->module, params->table, NULL );

    if (!entry || !entry->get_parameter) return STATUS_NOT_SUPPORTED;
    if (ios_nsi_check_get_parameter( entry->sizes, params->key_size, params->key, params->param_type,
                                     params->data, params->data_size, params->data_offset ))
        return STATUS_INVALID_PARAMETER;
    return entry->get_parameter( params->key, params->key_size, params->param_type,
                                 params->data, params->data_size, params->data_offset );
}

/* ---------------------------------------------------------------- nsi.dll's enumerate (cache, log) */

static pthread_mutex_t ios_nsi_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ios_nsi_cache ios_nsi_cache;
static unsigned int ios_nsi_logged;

#define IOS_NSI_LOG_MAX 16

static void ios_nsi_log_enumerate( const struct nsi_enumerate_all_ex *params, int index,
                                   NTSTATUS status, BOOL cached )
{
    unsigned int n = __atomic_fetch_add( &ios_nsi_logged, 1, __ATOMIC_RELAXED );

    if (n >= IOS_NSI_LOG_MAX) return;
    fprintf( stderr, "[nsi-ios] enumerate %s table %u: status %#x, count %lu%s%s\n",
             index >= 0 ? ios_nsi_module_names[index] : "(unknown module)", (unsigned int)params->table,
             (unsigned int)status, (unsigned long)params->count, cached ? " (cached)" : "",
             n + 1 == IOS_NSI_LOG_MAX ? " (no more enumerates will be logged)" : "" );
}

static NTSTATUS ios_nsi_enumerate_from_dll( struct nsi_enumerate_all_ex *params )
{
    const struct module_table *entry;
    struct ios_nsi_cache_key key;
    UINT sizes[4], ttl, cached_status;
    void *data[4];
    uint64_t count;
    int index = -1, hit = 0;
    NTSTATUS status;

    if ((status = ios_nsi_prepare_enumerate( params, &entry, &index, sizes, data )))
    {
        ios_nsi_log_enumerate( params, index, status, FALSE );
        return status;
    }

    /* the TCP tables are never cached: they change with every connection */
    ttl = index == IOS_NSI_TCP ? 0 : ios_nsi_cache_ms();
    memset( &key, 0, sizeof(key) );
    key.module = index;
    key.table = params->table;
    key.first_arg = params->first_arg;
    key.second_arg = params->second_arg;
    memcpy( key.sizes, sizes, sizeof(key.sizes) );
    key.want_rows = sizes[0] || sizes[1] || sizes[2] || sizes[3];

    if (ttl)
    {
        count = params->count;
        pthread_mutex_lock( &ios_nsi_lock );
        hit = ios_nsi_cache_lookup( &ios_nsi_cache, &key, ios_nsi_now_ms(), ttl, data, &count, &cached_status );
        pthread_mutex_unlock( &ios_nsi_lock );
        if (hit)
        {
            params->count = count;
            ios_nsi_log_enumerate( params, index, cached_status, TRUE );
            return cached_status;
        }
    }

    status = entry->enumerate_all( data[0], sizes[0], data[1], sizes[1], data[2], sizes[2],
                                   data[3], sizes[3], &params->count );

    if (ttl && status == STATUS_SUCCESS)   /* failures are never cached */
    {
        pthread_mutex_lock( &ios_nsi_lock );
        ios_nsi_cache_store( &ios_nsi_cache, &key, ios_nsi_now_ms(), ttl, data, params->count );
        pthread_mutex_unlock( &ios_nsi_lock );
    }
    ios_nsi_log_enumerate( params, index, status, FALSE );
    return status;
}

/* ---------------------------------------------------------------- the unix-call tables */

static NTSTATUS unix_nsi_enumerate_all_ex( void *args )
{
    return ios_nsi_enumerate_from_dll( args );
}

static NTSTATUS unix_nsi_get_all_parameters_ex( void *args )
{
    return nsi_get_all_parameters_ex( args );
}

static NTSTATUS unix_nsi_get_parameter_ex( void *args )
{
    return nsi_get_parameter_ex( args );
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_nsi_enumerate_all_ex,
    unix_nsi_get_all_parameters_ex,
    unix_nsi_get_parameter_ex,
};

#ifdef _WIN64

/* Part B: every pointer of the 32-bit structs is a guest address of the calling program
 * (ios_g2h: B + value, 0 stays NULL); the two unknown pointers are not used */

static NTSTATUS wow64_nsi_enumerate_all_ex( void *args )
{
    struct ios_nsi_enumerate_all_ex32 *params32 = args;
    struct nsi_enumerate_all_ex params;
    NTSTATUS status;

    params.unknown[0] = params.unknown[1] = NULL;
    params.module = ios_g2h( params32->module );
    params.table = params32->table;
    params.first_arg = params32->first_arg;
    params.second_arg = params32->second_arg;
    params.key_data = ios_g2h( params32->key_data );
    params.key_size = params32->key_size;
    params.rw_data = ios_g2h( params32->rw_data );
    params.rw_size = params32->rw_size;
    params.dynamic_data = ios_g2h( params32->dynamic_data );
    params.dynamic_size = params32->dynamic_size;
    params.static_data = ios_g2h( params32->static_data );
    params.static_size = params32->static_size;
    params.count = params32->count;

    status = ios_nsi_enumerate_from_dll( &params );
    params32->count = params.count;
    return status;
}

static NTSTATUS wow64_nsi_get_all_parameters_ex( void *args )
{
    const struct ios_nsi_get_all_parameters_ex32 *params32 = args;
    struct nsi_get_all_parameters_ex params;

    params.unknown[0] = params.unknown[1] = NULL;
    params.module = ios_g2h( params32->module );
    params.table = params32->table;
    params.first_arg = params32->first_arg;
    params.unknown2 = params32->unknown2;
    params.key = ios_g2h( params32->key );
    params.key_size = params32->key_size;
    params.rw_data = ios_g2h( params32->rw_data );
    params.rw_size = params32->rw_size;
    params.dynamic_data = ios_g2h( params32->dynamic_data );
    params.dynamic_size = params32->dynamic_size;
    params.static_data = ios_g2h( params32->static_data );
    params.static_size = params32->static_size;
    return nsi_get_all_parameters_ex( &params );
}

static NTSTATUS wow64_nsi_get_parameter_ex( void *args )
{
    const struct ios_nsi_get_parameter_ex32 *params32 = args;
    struct nsi_get_parameter_ex params;

    params.unknown[0] = params.unknown[1] = NULL;
    params.module = ios_g2h( params32->module );
    params.table = params32->table;
    params.first_arg = params32->first_arg;
    params.unknown2 = params32->unknown2;
    params.key = ios_g2h( params32->key );
    params.key_size = params32->key_size;
    params.param_type = params32->param_type;
    params.data = ios_g2h( params32->data );
    params.data_size = params32->data_size;
    params.data_offset = params32->data_offset;
    return nsi_get_parameter_ex( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_nsi_enumerate_all_ex,
    wow64_nsi_get_all_parameters_ex,
    wow64_nsi_get_parameter_ex,
};

#endif /* _WIN64 */
