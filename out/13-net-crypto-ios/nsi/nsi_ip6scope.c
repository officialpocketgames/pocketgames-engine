/*
 * nsiproxy.sys tcp module: the IPv6 address scope table
 *
 * Copyright 2003, 2006, 2011 Juan Lang
 * Copyright 2007 TransGaming Technologies Inc.
 * Copyright 2021 Huw Davies
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
 * Changed for MYIOSDECK (2026, spec 13 Part I5): get_ipv6_addr_scope_table() and
 * find_ipv6_addr_scope() taken out of dlls/nsiproxy.sys/tcp.c of Wine 11.4 (its getifaddrs
 * branch only), because ip.c needs them and tcp.c is not built. *size is set on every path (0
 * on failure) and interfaces without an address are skipped.
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
#include <netinet/in.h>
#include <ifaddrs.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ifdef.h"
#include "netiodef.h"
#include "ws2ipdef.h"
#include "wine/nsi.h"
#include "wine/debug.h"

#include "unix_private.h"

struct ipv6_addr_scope *get_ipv6_addr_scope_table( unsigned int *size )
{
    struct ipv6_addr_scope *table = NULL, *new_table;
    unsigned int table_size = 0, num = 0;
    struct ifaddrs *addrs, *cur;

    *size = 0;
    if (getifaddrs( &addrs ) == -1) return NULL;

    for (cur = addrs; cur; cur = cur->ifa_next)
    {
        struct sockaddr_in6 *sin6;
        struct ipv6_addr_scope *entry;

        if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET6) continue;

        if (++num > table_size)
        {
            if (!table_size) table_size = 4;
            else table_size *= 2;
            if (!(new_table = realloc( table, table_size * sizeof(table[0]) )))
            {
                freeifaddrs( addrs );
                free( table );
                return NULL;
            }
            table = new_table;
        }

        sin6 = (struct sockaddr_in6 *)cur->ifa_addr;
        entry = table + num - 1;
        memcpy( &entry->addr, &sin6->sin6_addr, sizeof(entry->addr) );
        entry->scope = sin6->sin6_scope_id;
    }

    freeifaddrs( addrs );
    *size = num;
    return table;
}

UINT find_ipv6_addr_scope( const IN6_ADDR *addr, const struct ipv6_addr_scope *table, unsigned int size )
{
    const BYTE multicast_scope_mask = 0x0F;
    const BYTE multicast_scope_shift = 0;
    unsigned int i;

    if (WS_IN6_IS_ADDR_UNSPECIFIED( addr )) return 0;

    if (WS_IN6_IS_ADDR_MULTICAST( addr ))
        return htons( (addr->u.Byte[1] & multicast_scope_mask) >> multicast_scope_shift );

    if (!table) return -1;

    for (i = 0; i < size; i++)
        if (!memcmp( &table[i].addr, addr, sizeof(table[i].addr) ))
            return table[i].scope;

    return -1;
}
