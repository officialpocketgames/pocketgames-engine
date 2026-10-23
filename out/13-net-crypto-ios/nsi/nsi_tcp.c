/*
 * nsi.dll's unix side for iOS: the TCP connection tables
 *
 * Copyright 2003, 2006, 2011 Juan Lang
 * Copyright 2007 TransGaming Technologies Inc.
 * Copyright 2021 Huw Davies
 * Copyright (c) 2026 the MYIOSDECK author
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
 *
 * Written for MYIOSDECK (2026, spec 13 Part I2). The rows are built as tcp_conns_enumerate_all()
 * of dlls/nsiproxy.sys/tcp.c (Wine 11.4) builds them, from the wineserver's get_tcp_connections
 * request: key rows from the addresses and ports, dynamic rows the state, static rows the
 * owner's process id. The IPv6 scope ids come from the server's reply. Only NSI_TCP_ALL_TABLE,
 * NSI_TCP_ESTAB_TABLE and NSI_TCP_LISTEN_TABLE are served.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ifdef.h"
#include "netiodef.h"
#include "ws2ipdef.h"
#include "tcpmib.h"
#include "wine/nsi.h"
#include "wine/debug.h"
#include "wine/server.h"

#include "unix_private.h"
#include "ios_nsi_pure.h"

C_ASSERT( IOS_NSI_TCP_ALL_TABLE == NSI_TCP_ALL_TABLE );
C_ASSERT( IOS_NSI_TCP_ESTAB_TABLE == NSI_TCP_ESTAB_TABLE );
C_ASSERT( IOS_NSI_TCP_LISTEN_TABLE == NSI_TCP_LISTEN_TABLE );
C_ASSERT( IOS_MIB_TCP_STATE_ESTAB == MIB_TCP_STATE_ESTAB );
C_ASSERT( IOS_MIB_TCP_STATE_LISTEN == MIB_TCP_STATE_LISTEN );

/* The dispatcher (nsi_unix.c) has checked the sizes: each of key, dynamic and static is 0 or
 * exactly its struct's size, rw is 0, and a non-zero size has a buffer. The request goes out on
 * the calling thread's own server connection. */
static NTSTATUS tcp_conns_enumerate_all( UINT table, struct nsi_tcp_conn_key *key_data, UINT key_size,
                                         void *rw, UINT rw_size,
                                         struct nsi_tcp_conn_dynamic *dynamic_data, UINT dynamic_size,
                                         struct nsi_tcp_conn_static *static_data, UINT static_size,
                                         UINT_PTR *count )
{
    BOOL want_data = key_size || rw_size || dynamic_size || static_size;
    int filter = ios_nsi_tcp_state_filter( table );
    union tcp_connection *connections = NULL;
    UINT room = 0, server_count = 0, i;
    uint64_t rows = *count;
    NTSTATUS status;

    if (filter < 0) return STATUS_NOT_SUPPORTED;

    if (want_data && *count)
    {
        /* the caller's room, bounded only so the reply buffer size can't overflow */
        room = *count > (1u << 20) ? (1u << 20) : *count;
        if (!(connections = malloc( sizeof(*connections) * room ))) return STATUS_NO_MEMORY;
    }

    SERVER_START_REQ( get_tcp_connections )
    {
        req->state_filter = filter;
        wine_server_set_reply( req, connections, sizeof(*connections) * room );
        status = wine_server_call( req );
        server_count = reply->count;
    }
    SERVER_END_REQ;

    status = ios_nsi_tcp_outcome( want_data, status, server_count, &rows );
    if (status || !want_data)
    {
        if (!status) *count = rows;
        free( connections );
        return status;
    }
    if (rows > room) rows = room;   /* never more rows than the buffers hold */
    *count = rows;

    for (i = 0; i < rows; i++)
    {
        const union tcp_connection *conn = &connections[i];

        if (key_data)
        {
            struct nsi_tcp_conn_key key;

            memset( &key, 0, sizeof(key) );
            if (conn->common.family == WS_AF_INET)
            {
                key.local.Ipv4.sin_family = key.remote.Ipv4.sin_family = WS_AF_INET;
                key.local.Ipv4.sin_addr.WS_s_addr = conn->ipv4.local_addr;
                key.local.Ipv4.sin_port = conn->ipv4.local_port;
                key.remote.Ipv4.sin_addr.WS_s_addr = conn->ipv4.remote_addr;
                key.remote.Ipv4.sin_port = conn->ipv4.remote_port;
            }
            else
            {
                key.local.Ipv6.sin6_family = key.remote.Ipv6.sin6_family = WS_AF_INET6;
                memcpy( &key.local.Ipv6.sin6_addr, conn->ipv6.local_addr, 16 );
                key.local.Ipv6.sin6_port = conn->ipv6.local_port;
                key.local.Ipv6.sin6_scope_id = conn->ipv6.local_scope_id;
                memcpy( &key.remote.Ipv6.sin6_addr, conn->ipv6.remote_addr, 16 );
                key.remote.Ipv6.sin6_port = conn->ipv6.remote_port;
                key.remote.Ipv6.sin6_scope_id = conn->ipv6.remote_scope_id;
            }
            key_data[i] = key;
        }

        if (dynamic_data)
        {
            struct nsi_tcp_conn_dynamic dyn;

            memset( &dyn, 0, sizeof(dyn) );
            dyn.state = conn->common.state;
            dynamic_data[i] = dyn;
        }

        if (static_data)
        {
            struct nsi_tcp_conn_static stat;

            memset( &stat, 0, sizeof(stat) );
            stat.pid = conn->common.owner;   /* the socket's real owner, known to the server */
            stat.create_time = 0;
            stat.mod_info = 0;
            static_data[i] = stat;
        }
    }

    free( connections );
    return STATUS_SUCCESS;
}

static NTSTATUS tcp_all_enumerate_all( void *key_data, UINT key_size, void *rw_data, UINT rw_size,
                                       void *dynamic_data, UINT dynamic_size,
                                       void *static_data, UINT static_size, UINT_PTR *count )
{
    return tcp_conns_enumerate_all( NSI_TCP_ALL_TABLE, key_data, key_size, rw_data, rw_size,
                                    dynamic_data, dynamic_size, static_data, static_size, count );
}

static NTSTATUS tcp_estab_enumerate_all( void *key_data, UINT key_size, void *rw_data, UINT rw_size,
                                         void *dynamic_data, UINT dynamic_size,
                                         void *static_data, UINT static_size, UINT_PTR *count )
{
    return tcp_conns_enumerate_all( NSI_TCP_ESTAB_TABLE, key_data, key_size, rw_data, rw_size,
                                    dynamic_data, dynamic_size, static_data, static_size, count );
}

static NTSTATUS tcp_listen_enumerate_all( void *key_data, UINT key_size, void *rw_data, UINT rw_size,
                                          void *dynamic_data, UINT dynamic_size,
                                          void *static_data, UINT static_size, UINT_PTR *count )
{
    return tcp_conns_enumerate_all( NSI_TCP_LISTEN_TABLE, key_data, key_size, rw_data, rw_size,
                                    dynamic_data, dynamic_size, static_data, static_size, count );
}

static const struct module_table tcp_tables[] =
{
    {
        NSI_TCP_ALL_TABLE,
        {
            sizeof(struct nsi_tcp_conn_key), 0,
            sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static)
        },
        tcp_all_enumerate_all,
    },
    {
        NSI_TCP_ESTAB_TABLE,
        {
            sizeof(struct nsi_tcp_conn_key), 0,
            sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static)
        },
        tcp_estab_enumerate_all,
    },
    {
        NSI_TCP_LISTEN_TABLE,
        {
            sizeof(struct nsi_tcp_conn_key), 0,
            sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static)
        },
        tcp_listen_enumerate_all,
    },
    {
        ~0u
    }
};

const struct module tcp_module =
{
    &NPI_MS_TCP_MODULEID,
    tcp_tables
};
