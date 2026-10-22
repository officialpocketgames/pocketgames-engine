/*
 * Network Store Interface
 *
 * Copyright 2021 Huw Davies
 *
 * Changed for MYIOSDECK (spec 19 Part A9): without \\.\Nsi the tables come from nsi.dll's own
 * unix-call table (spec 13 Part I1), the missing device is remembered, and change requests stay
 * pending (nsi_fallback.h).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include "winsock2.h"
#include "winternl.h"
#include "ws2ipdef.h"
#include "iphlpapi.h"
#include "netioapi.h"
#include "iptypes.h"
#include "netiodef.h"
#include "wine/nsi.h"
#include "wine/unixlib.h"
#include "wine/debug.h"

#define NSI_OVL OVERLAPPED
#define NSI_HANDLE HANDLE
#define NSI_RESET_EVENT(h) ResetEvent( h )
#define NSI_SET_EVENT(h) SetEvent( h )
#include "nsi_fallback.h"

WINE_DEFAULT_DEBUG_CHANNEL(nsi);

static HANDLE nsi_device = INVALID_HANDLE_VALUE;
static HANDLE nsi_device_async = INVALID_HANDLE_VALUE;

/* MYIOSDECK (spec 19 Part A9): without nsiproxy.sys, \\.\Nsi can't be opened; the tables come
 * from nsi.dll's own unix-call table instead (spec 13 Part I). */

/* NPI_MS_NDIS_MODULEID's value (netiodef.h): only nsiproxy instantiates it upstream */
static const NPI_MODULEID nsi_ndis_module =
{
    sizeof(NPI_MODULEID), MIT_GUID,
    { { 0xeb004a11, 0x9b1a, 0x11d4, { 0x91, 0x23, 0x00, 0x50, 0x04, 0x77, 0x59, 0xbc } } }
};

/* the error each kind of handle (0 synchronous, 1 overlapped) failed to open with, once it is
 * remembered (item 3); 0 while not */
static LONG nsi_missing_device_error[2];
/* the event a pending change request hands out; nothing ever sets it (item 6) */
static HANDLE nsi_never_set_event;

static BOOL CALLBACK nsi_init_fallback( INIT_ONCE *once, void *param, void **context )
{
    NTSTATUS status = __wine_init_unix_call();

    if (status) TRACE( "no unix-call table: %#lx\n", status );
    *(BOOL *)param = !status;
    return TRUE;
}

/* item 1: the unix-call table exists; asked once per process */
static BOOL nsi_fallback_available(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static BOOL available;

    InitOnceExecuteOnce( &once, nsi_init_fallback, &available, NULL );
    return available;
}

static BOOL CALLBACK nsi_init_parameters_probe( INIT_ONCE *once, void *param, void **context )
{
    struct nsi_enumerate_all_ex probe;
    NTSTATUS status;

    memset( &probe, 0, sizeof(probe) );
    probe.module = &nsi_ndis_module;
    probe.table = NSI_NDIS_IFINFO_TABLE;
    status = WINE_UNIX_CALL( NSI_UNIX_ENUMERATE_ALL, &probe );
    *(BOOL *)param = nsi_probe_allows_parameters( status );
    ERR( "unix-call table: NDIS interface count probe %#lx (count %Iu), parameter reads %s\n",
         status, probe.count, *(BOOL *)param ? "on" : "off" );
    return TRUE;
}

/* item 2: entries 1 and 2 exist; asked once per process (only with the table present) */
static BOOL nsi_parameters_available(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static BOOL available;

    InitOnceExecuteOnce( &once, nsi_init_parameters_probe, &available, NULL );
    return available;
}

/* item 7: is a switch on? (an unset or empty variable counts as on) */
static BOOL nsi_read_switch( const char *name )
{
    char small[64], *value = small;
    DWORD len = GetEnvironmentVariableA( name, small, sizeof(small) );
    BOOL on;

    if (len >= sizeof(small) && (value = malloc( len )))
        len = GetEnvironmentVariableA( name, value, len );
    on = nsi_switch_on( value && len ? value : NULL );
    if (value && value != small) free( value );
    return on;
}

static BOOL CALLBACK nsi_init_switches( INIT_ONCE *once, void *param, void **context )
{
    BOOL *switches = param;

    switches[0] = nsi_read_switch( "MADEIRA_NSI_DEVICE_CACHE" );
    switches[1] = nsi_read_switch( "MADEIRA_NSI_NOTIFY_PENDING" );
    return TRUE;
}

/* item 7: each switch read once per process; 0 the device cache, 1 pending notifications */
static BOOL nsi_switch( int which )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static BOOL switches[2];

    InitOnceExecuteOnce( &once, nsi_init_switches, switches, NULL );
    return switches[which];
}

/* item 4: entry 0 for a caller with no device */
static DWORD nsi_fallback_enumerate( struct nsi_enumerate_all_ex *params, DWORD device_error )
{
    static LONG logged;
    NTSTATUS status = WINE_UNIX_CALL( NSI_UNIX_ENUMERATE_ALL, params );

    if (!InterlockedExchange( &logged, 1 ))
        ERR( "no \\\\.\\Nsi (error %lu): enumerating through the unix-call table, first status %#lx\n",
             device_error, status );
    switch (nsi_map_enumerate_status( status ))
    {
    case NSI_MAP_MORE_DATA: return ERROR_MORE_DATA;
    case NSI_MAP_DEVICE_ERROR: return device_error;
    default: return RtlNtStatusToDosError( status );
    }
}

/* item 5: entry 1 or 2 for a caller with no device */
static DWORD nsi_fallback_parameters( enum nsi_unix_entry entry, void *params, DWORD device_error )
{
    NTSTATUS status;

    if (!nsi_fallback_available() || !nsi_parameters_available()) return device_error;
    status = WINE_UNIX_CALL( entry, params );
    switch (nsi_map_parameter_status( status ))
    {
    case NSI_MAP_DEVICE_ERROR: return device_error;
    default: return RtlNtStatusToDosError( status );
    }
}

static BOOL CALLBACK nsi_init_never_set_event( INIT_ONCE *once, void *param, void **context )
{
    nsi_never_set_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    return TRUE;
}

/* item 6: a change request that stays pending */
static DWORD nsi_pending_change_request( struct nsi_request_change_notification_ex *params )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static LONG logged;

    InitOnceExecuteOnce( &once, nsi_init_never_set_event, NULL, NULL );
    if (!nsi_never_set_event) return GetLastError();
    if (!InterlockedExchange( &logged, 1 ))
        ERR( "no \\\\.\\Nsi: change requests stay pending (the served tables don't change)\n" );

    if (!params->ovr)
    {
        /* as a caller waiting for a change would */
        WaitForSingleObject( nsi_never_set_event, INFINITE );
        return ERROR_SUCCESS;
    }
    if (params->handle) *params->handle = nsi_never_set_event;
    return nsi_pending_request( params->ovr );
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, void *reserved)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls( hinst );
            break;
        case DLL_PROCESS_DETACH:
            if (nsi_device != INVALID_HANDLE_VALUE) CloseHandle( nsi_device );
            if (nsi_device_async != INVALID_HANDLE_VALUE) CloseHandle( nsi_device_async );
            break;
    }
    return TRUE;
}

static inline HANDLE get_nsi_device( BOOL async )
{
    HANDLE *cached_device = async ? &nsi_device_async : &nsi_device;
    LONG *missing = &nsi_missing_device_error[async ? 1 : 0];
    HANDLE device;

    if (*cached_device == INVALID_HANDLE_VALUE)
    {
        DWORD err;

        /* MYIOSDECK (spec 19 Part A9.3): a missing device is remembered, with its error */
        if ((err = ReadNoFence( missing )))
        {
            SetLastError( err );
            return INVALID_HANDLE_VALUE;
        }
        device = CreateFileW( L"\\\\.\\Nsi", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              async ? FILE_FLAG_OVERLAPPED : 0, NULL );
        if (device == INVALID_HANDLE_VALUE)
        {
            static LONG logged;

            err = GetLastError();
            if (nsi_remember_missing_device( err, nsi_fallback_available(), nsi_switch( 0 ) ))
            {
                InterlockedExchange( missing, err );
                if (!InterlockedExchange( &logged, 1 ))
                    ERR( "\\\\.\\Nsi doesn't exist (error %lu); not trying again in this process\n", err );
            }
            SetLastError( err );
            return INVALID_HANDLE_VALUE;
        }
        if (device != INVALID_HANDLE_VALUE
            && InterlockedCompareExchangePointer( cached_device, device, INVALID_HANDLE_VALUE ) != INVALID_HANDLE_VALUE)
            CloseHandle( device );
    }
    return *cached_device;
}

DWORD WINAPI NsiAllocateAndGetTable( DWORD unk, const NPI_MODULEID *module, DWORD table, void **key_data, DWORD key_size,
                                     void **rw_data, DWORD rw_size, void **dynamic_data, DWORD dynamic_size,
                                     void **static_data, DWORD static_size, DWORD *count, DWORD unk2 )
{
    DWORD err, num = 64;
    void *data[4] = { NULL };
    DWORD sizes[4] = { key_size, rw_size, dynamic_size, static_size };
    int i, attempt;

    TRACE( "%ld %p %ld %p %ld %p %ld %p %ld %p %ld %p %ld\n", unk, module, table, key_data, key_size,
           rw_data, rw_size, dynamic_data, dynamic_size, static_data, static_size, count, unk2 );

    for (attempt = 0; attempt < 5; attempt++)
    {
        for (i = 0; i < ARRAY_SIZE(data); i++)
        {
            if (sizes[i])
            {
                data[i] = HeapAlloc( GetProcessHeap(), 0, sizes[i] * num );
                if (!data[i])
                {
                    err = ERROR_OUTOFMEMORY;
                    goto err;
                }
            }
        }

        err = NsiEnumerateObjectsAllParameters( unk, 0, module, table, data[0], sizes[0], data[1], sizes[1],
                                                data[2], sizes[2], data[3], sizes[3], &num );
        if (err != ERROR_MORE_DATA) break;
        TRACE( "Short buffer, attempt %d.\n", attempt );
        NsiFreeTable( data[0], data[1], data[2], data[3] );
        memset( data, 0, sizeof(data) );
        err = NsiEnumerateObjectsAllParameters( unk, 0, module, table, NULL, 0, NULL, 0, NULL, 0, NULL, 0, &num );
        if (err) return err;
        err = ERROR_OUTOFMEMORY; /* fail if this is the last attempt */
        num += num >> 4; /* the tables may grow before the next iteration; get ahead */
    }

    if (!err)
    {
        if (sizes[0]) *key_data = data[0];
        if (sizes[1]) *rw_data = data[1];
        if (sizes[2]) *dynamic_data = data[2];
        if (sizes[3]) *static_data = data[3];
        *count = num;
    }

err:
    if (err) NsiFreeTable( data[0], data[1], data[2], data[3] );
    return err;
}

DWORD WINAPI NsiCancelChangeNotification( OVERLAPPED *ovr )
{
    DWORD err = ERROR_SUCCESS;

    TRACE( "%p.\n", ovr );

    if (!ovr) return ERROR_NOT_FOUND;
    /* MYIOSDECK (spec 19 Part A9.6): a request that stayed pending because there is no device */
    if (nsi_never_set_event && get_nsi_device( TRUE ) == INVALID_HANDLE_VALUE)
        return nsi_pending_cancel( ovr );
    if (!CancelIoEx(  get_nsi_device( TRUE ), ovr ))
        err = GetLastError();

    return err;
}

DWORD WINAPI NsiEnumerateObjectsAllParameters( DWORD unk, DWORD unk2, const NPI_MODULEID *module, DWORD table,
                                               void *key_data, DWORD key_size, void *rw_data, DWORD rw_size,
                                               void *dynamic_data, DWORD dynamic_size, void *static_data, DWORD static_size,
                                               DWORD *count )
{
    struct nsi_enumerate_all_ex params;
    DWORD err;

    TRACE( "%ld %ld %p %ld %p %ld %p %ld %p %ld %p %ld %p\n", unk, unk2, module, table, key_data, key_size,
           rw_data, rw_size, dynamic_data, dynamic_size, static_data, static_size, count );

    params.unknown[0] = 0;
    params.unknown[1] = 0;
    params.module = module;
    params.table = table;
    params.first_arg = unk;
    params.second_arg = unk2;
    params.key_data = key_data;
    params.key_size = key_size;
    params.rw_data = rw_data;
    params.rw_size = rw_size;
    params.dynamic_data = dynamic_data;
    params.dynamic_size = dynamic_size;
    params.static_data = static_data;
    params.static_size = static_size;
    params.count = *count;

    err = NsiEnumerateObjectsAllParametersEx( &params );
    *count = params.count;
    return err;
}

DWORD WINAPI NsiEnumerateObjectsAllParametersEx( struct nsi_enumerate_all_ex *params )
{
    DWORD out_size, received, err = ERROR_SUCCESS;
    HANDLE device = get_nsi_device( FALSE );
    struct nsiproxy_enumerate_all in;
    BYTE *out, *ptr;

    if (device == INVALID_HANDLE_VALUE)
    {
        /* MYIOSDECK (spec 19 Part A9.4) */
        DWORD device_error = GetLastError();

        if (!nsi_fallback_available()) return device_error;
        return nsi_fallback_enumerate( params, device_error );
    }

    out_size = sizeof(DWORD) +
        (params->key_size + params->rw_size + params->dynamic_size + params->static_size) * params->count;

    out = malloc( out_size );
    if (!out) return ERROR_OUTOFMEMORY;

    in.module = *params->module;
    in.first_arg = params->first_arg;
    in.second_arg = params->second_arg;
    in.table = params->table;
    in.key_size = params->key_size;
    in.rw_size = params->rw_size;
    in.dynamic_size = params->dynamic_size;
    in.static_size = params->static_size;
    in.count = params->count;

    if (!DeviceIoControl( device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, out_size, &received, NULL ))
        err = GetLastError();
    if (err == ERROR_SUCCESS || err == ERROR_MORE_DATA)
    {
        params->count = *(DWORD *)out;
        ptr = out + sizeof(DWORD);
        if (params->key_size) memcpy( params->key_data, ptr, params->key_size * params->count );
        ptr += params->key_size * in.count;
        if (params->rw_size) memcpy( params->rw_data, ptr, params->rw_size * params->count );
        ptr += params->rw_size * in.count;
        if (params->dynamic_size) memcpy( params->dynamic_data, ptr, params->dynamic_size * params->count );
        ptr += params->dynamic_size * in.count;
        if (params->static_size) memcpy( params->static_data, ptr, params->static_size * params->count );
    }

    free( out );

    return err;
}

void WINAPI NsiFreeTable( void *key_data, void *rw_data, void *dynamic_data, void *static_data )
{
    TRACE( "%p %p %p %p\n", key_data, rw_data, dynamic_data, static_data );
    HeapFree( GetProcessHeap(), 0, key_data );
    HeapFree( GetProcessHeap(), 0, rw_data );
    HeapFree( GetProcessHeap(), 0, dynamic_data );
    HeapFree( GetProcessHeap(), 0, static_data );
}

DWORD WINAPI NsiGetAllParameters( DWORD unk, const NPI_MODULEID *module, DWORD table, const void *key, DWORD key_size,
                                  void *rw_data, DWORD rw_size, void *dynamic_data, DWORD dynamic_size,
                                  void *static_data, DWORD static_size )
{
    struct nsi_get_all_parameters_ex params;

    TRACE( "%ld %p %ld %p %ld %p %ld %p %ld %p %ld\n", unk, module, table, key, key_size,
           rw_data, rw_size, dynamic_data, dynamic_size, static_data, static_size );

    params.unknown[0] = 0;
    params.unknown[1] = 0;
    params.module = module;
    params.table = table;
    params.first_arg = unk;
    params.unknown2 = 0;
    params.key = key;
    params.key_size = key_size;
    params.rw_data = rw_data;
    params.rw_size = rw_size;
    params.dynamic_data = dynamic_data;
    params.dynamic_size = dynamic_size;
    params.static_data = static_data;
    params.static_size = static_size;

    return NsiGetAllParametersEx( &params );
}

DWORD WINAPI NsiGetAllParametersEx( struct nsi_get_all_parameters_ex *params )
{
    HANDLE device = get_nsi_device( FALSE );
    struct nsiproxy_get_all_parameters *in;
    ULONG in_size = FIELD_OFFSET( struct nsiproxy_get_all_parameters, key[params->key_size] ), received;
    ULONG out_size = params->rw_size + params->dynamic_size + params->static_size;
    DWORD err = ERROR_SUCCESS;
    BYTE *out, *ptr;

    /* MYIOSDECK (spec 19 Part A9.5) */
    if (device == INVALID_HANDLE_VALUE)
        return nsi_fallback_parameters( NSI_UNIX_GET_ALL_PARAMETERS, params, GetLastError() );

    in = malloc( in_size );
    out = malloc( out_size );
    if (!in || !out)
    {
        err = ERROR_OUTOFMEMORY;
        goto err;
    }

    in->module = *params->module;
    in->first_arg = params->first_arg;
    in->table = params->table;
    in->key_size = params->key_size;
    in->rw_size = params->rw_size;
    in->dynamic_size = params->dynamic_size;
    in->static_size = params->static_size;
    memcpy( in->key, params->key, params->key_size );

    if (!DeviceIoControl( device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, in, in_size, out, out_size, &received, NULL ))
        err = GetLastError();
    if (err == ERROR_SUCCESS)
    {
        ptr = out;
        if (params->rw_size) memcpy( params->rw_data, ptr, params->rw_size );
        ptr += params->rw_size;
        if (params->dynamic_size) memcpy( params->dynamic_data, ptr, params->dynamic_size );
        ptr += params->dynamic_size;
        if (params->static_size) memcpy( params->static_data, ptr, params->static_size );
    }

err:
    free( out );
    free( in );
    return err;
}

DWORD WINAPI NsiGetParameter( DWORD unk, const NPI_MODULEID *module, DWORD table, const void *key, DWORD key_size,
                              DWORD param_type, void *data, DWORD data_size, DWORD data_offset )
{
    struct nsi_get_parameter_ex params;

    TRACE( "%ld %p %ld %p %ld %ld %p %ld %ld\n", unk, module, table, key, key_size,
           param_type, data, data_size, data_offset );

    params.unknown[0] = 0;
    params.unknown[1] = 0;
    params.module = module;
    params.table = table;
    params.first_arg = unk;
    params.unknown2 = 0;
    params.key = key;
    params.key_size = key_size;
    params.param_type = param_type;
    params.data = data;
    params.data_size = data_size;
    params.data_offset = data_offset;
    return NsiGetParameterEx( &params );
}

DWORD WINAPI NsiGetParameterEx( struct nsi_get_parameter_ex *params )
{
    HANDLE device = get_nsi_device( FALSE );
    struct nsiproxy_get_parameter *in;
    ULONG in_size = FIELD_OFFSET( struct nsiproxy_get_parameter, key[params->key_size] ), received;
    DWORD err = ERROR_SUCCESS;

    /* MYIOSDECK (spec 19 Part A9.5) */
    if (device == INVALID_HANDLE_VALUE)
        return nsi_fallback_parameters( NSI_UNIX_GET_PARAMETER, params, GetLastError() );

    in = malloc( in_size );
    if (!in) return ERROR_OUTOFMEMORY;
    in->module = *params->module;
    in->first_arg = params->first_arg;
    in->table = params->table;
    in->key_size = params->key_size;
    in->param_type = params->param_type;
    in->data_offset = params->data_offset;
    memcpy( in->key, params->key, params->key_size );

    if (!DeviceIoControl( device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, in, in_size, params->data, params->data_size, &received, NULL ))
        err = GetLastError();

    free( in );
    return err;
}

DWORD WINAPI NsiRequestChangeNotification( DWORD unk, const NPI_MODULEID *module, DWORD table, OVERLAPPED *ovr,
                                           HANDLE *handle )
{
    struct nsi_request_change_notification_ex params;

    TRACE( "%lu %p %lu %p %p stub.\n", unk, module, table, ovr, handle );

    params.unk = unk;
    params.module = module;
    params.table = table;
    params.ovr = ovr;
    params.handle = handle;
    return NsiRequestChangeNotificationEx( &params );
}

DWORD WINAPI NsiRequestChangeNotificationEx( struct nsi_request_change_notification_ex *params )
{
    HANDLE device = get_nsi_device( TRUE );
    struct nsiproxy_request_notification *in;
    ULONG in_size = sizeof(struct nsiproxy_get_parameter), received;
    OVERLAPPED overlapped, *ovr;
    DWORD err = ERROR_SUCCESS;
    DWORD len;

    TRACE( "%p.\n", params );

    if (params->unk) FIXME( "unknown parameter %#lx.\n", params->unk );

    if (device == INVALID_HANDLE_VALUE)
    {
        /* MYIOSDECK (spec 19 Part A9.6): the fallback's addresses don't change during a session,
         * and callers that see the request fail re-read every table in a tight loop */
        DWORD device_error = GetLastError();

        if (nsi_notify_stays_pending( device_error, nsi_fallback_available(), nsi_switch( 1 ) ))
            return nsi_pending_change_request( params );
        return device_error;
    }

    in = malloc( in_size );
    if (!in) return ERROR_OUTOFMEMORY;
    in->module = *params->module;
    in->table = params->table;

    if (!(ovr = params->ovr))
    {
        overlapped.hEvent = CreateEventW( NULL, FALSE, FALSE, NULL );
        ovr = &overlapped;
    }
    if (!DeviceIoControl( device, IOCTL_NSIPROXY_WINE_CHANGE_NOTIFICATION, in, in_size, NULL, 0, &received, ovr ))
        err = GetLastError();
    if (ovr == &overlapped)
    {
        if (err == ERROR_IO_PENDING)
            err = GetOverlappedResult( device, ovr, &len, TRUE ) ? 0 : GetLastError();
        CloseHandle( overlapped.hEvent );
    }
    else if (params->handle && ovr && err == ERROR_IO_PENDING)
        *params->handle = device;

    free( in );
    return err;
}
