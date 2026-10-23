// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part I: pure logic of nsi.dll's unix-call table. See include/ios_nsi_pure.h. The
// cache has no lock of its own: nsi/nsi_unix.c calls it under its mutex.

#include <stdlib.h>
#include <string.h>

#include "ios_nsi_pure.h"

/* ---------------------------------------------------------------- I3: parameter checks */

static uint32_t check_part( uint32_t table_size, uint32_t size, void *data, void **out )
{
    *out = NULL;
    if (!size) return IOS_NSI_STATUS_SUCCESS;
    if (size != table_size || !data) return IOS_NSI_STATUS_INVALID_PARAMETER;
    *out = data;
    return IOS_NSI_STATUS_SUCCESS;
}

uint32_t ios_nsi_check_enumerate( const uint32_t table_sizes[4], const uint32_t sizes[4],
                                  void *const data[4], void *out[4] )
{
    int i;

    for (i = 0; i < 4; i++)
        if (check_part( table_sizes[i], sizes[i], data[i], &out[i] )) return IOS_NSI_STATUS_INVALID_PARAMETER;
    return IOS_NSI_STATUS_SUCCESS;
}

uint32_t ios_nsi_check_get_all( const uint32_t table_sizes[4], const uint32_t sizes[4],
                                void *const data[4], void *out[4] )
{
    int i;

    /* the key is always the table's size (0 for tables without a key) */
    if (sizes[0] != table_sizes[0]) return IOS_NSI_STATUS_INVALID_PARAMETER;
    if (sizes[0] && !data[0]) return IOS_NSI_STATUS_INVALID_PARAMETER;
    out[0] = data[0];
    for (i = 1; i < 4; i++)
        if (check_part( table_sizes[i], sizes[i], data[i], &out[i] )) return IOS_NSI_STATUS_INVALID_PARAMETER;
    return IOS_NSI_STATUS_SUCCESS;
}

uint32_t ios_nsi_check_get_parameter( const uint32_t table_sizes[4], uint32_t key_size,
                                      const void *key, uint64_t param_type,
                                      const void *data, uint32_t data_size, uint32_t data_offset )
{
    uint64_t row;

    if (param_type > 2) return IOS_NSI_STATUS_INVALID_PARAMETER;
    if (key_size != table_sizes[0]) return IOS_NSI_STATUS_INVALID_PARAMETER;
    if (key_size && !key) return IOS_NSI_STATUS_INVALID_PARAMETER;
    if (data_size && !data) return IOS_NSI_STATUS_INVALID_PARAMETER;
    row = table_sizes[param_type + 1];
    if (data_offset > row || (uint64_t)data_offset + data_size > row) return IOS_NSI_STATUS_INVALID_PARAMETER;
    return IOS_NSI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- I2: TCP tables */

int ios_nsi_tcp_state_filter( uint32_t table )
{
    switch (table)
    {
    case IOS_NSI_TCP_ALL_TABLE:    return 0;
    case IOS_NSI_TCP_ESTAB_TABLE:  return IOS_MIB_TCP_STATE_ESTAB;
    case IOS_NSI_TCP_LISTEN_TABLE: return IOS_MIB_TCP_STATE_LISTEN;
    default:                       return -1;
    }
}

uint32_t ios_nsi_tcp_outcome( int want_data, uint32_t server_status, uint32_t server_count,
                              uint64_t *count )
{
    if (server_status == IOS_NSI_STATUS_SUCCESS)
    {
        *count = server_count;
        return IOS_NSI_STATUS_SUCCESS;
    }
    if (server_status == IOS_NSI_STATUS_BUFFER_TOO_SMALL)
    {
        /* with buffers, the outgoing count is never above the incoming one */
        if (want_data) return IOS_NSI_STATUS_BUFFER_OVERFLOW;
        *count = server_count;
        return IOS_NSI_STATUS_SUCCESS;
    }
    return server_status;
}

/* ---------------------------------------------------------------- I4: the cache */

uint32_t ios_nsi_cache_ttl_from_env( const char *value )
{
    unsigned long ms;
    char *end;

    if (!value || !value[0] || value[0] < '0' || value[0] > '9') return IOS_NSI_CACHE_DEFAULT_MS;
    ms = strtoul( value, &end, 10 );
    if (*end) return IOS_NSI_CACHE_DEFAULT_MS;
    return ms > IOS_NSI_CACHE_MAX_MS ? IOS_NSI_CACHE_MAX_MS : (uint32_t)ms;
}

static int same_key( const struct ios_nsi_cache_key *a, const struct ios_nsi_cache_key *b )
{
    return a->module == b->module && a->table == b->table && a->first_arg == b->first_arg &&
           a->second_arg == b->second_arg && a->want_rows == b->want_rows &&
           !memcmp( a->sizes, b->sizes, sizeof(a->sizes) );
}

int ios_nsi_cache_slot_of( const struct ios_nsi_cache *c, const struct ios_nsi_cache_key *key )
{
    int i;

    for (i = 0; i < IOS_NSI_CACHE_SLOTS; i++)
        if (c->slot[i].used && same_key( &c->slot[i].key, key )) return i;
    return -1;
}

static void free_entry( struct ios_nsi_cache_entry *e )
{
    int i;

    for (i = 0; i < 4; i++) free( e->rows[i] );
    memset( e, 0, sizeof(*e) );
}

int ios_nsi_cache_lookup( struct ios_nsi_cache *c, const struct ios_nsi_cache_key *key,
                          uint64_t now_ms, uint32_t ttl_ms, void *const data[4],
                          uint64_t *count, uint32_t *status )
{
    struct ios_nsi_cache_entry *e;
    uint64_t rows;
    int i = ios_nsi_cache_slot_of( c, key );

    if (!ttl_ms || i < 0) return 0;
    e = &c->slot[i];
    if (now_ms - e->stored_ms >= ttl_ms) return 0;

    if (!key->want_rows)
    {
        *count = e->count;
        *status = IOS_NSI_STATUS_SUCCESS;
        return 1;
    }

    rows = e->count <= *count ? e->count : *count;
    for (i = 0; i < 4; i++)
        if (key->sizes[i] && data[i] && rows) memcpy( data[i], e->rows[i], rows * key->sizes[i] );
    if (e->count <= *count)
    {
        *count = e->count;
        *status = IOS_NSI_STATUS_SUCCESS;
    }
    else *status = IOS_NSI_STATUS_BUFFER_OVERFLOW;
    return 1;
}

void ios_nsi_cache_store( struct ios_nsi_cache *c, const struct ios_nsi_cache_key *key,
                          uint64_t now_ms, uint32_t ttl_ms, void *const data[4], uint64_t count )
{
    struct ios_nsi_cache_entry *e = NULL, fresh;
    uint64_t bytes = 0;
    int i;

    if (!ttl_ms) return;

    if ((i = ios_nsi_cache_slot_of( c, key )) >= 0) e = &c->slot[i];

    if (key->want_rows)
        for (i = 0; i < 4; i++) bytes += count * key->sizes[i];
    if (bytes > IOS_NSI_CACHE_MAX_BYTES)
    {
        if (e) free_entry( e );   /* don't keep answering with an older result */
        return;
    }

    memset( &fresh, 0, sizeof(fresh) );
    fresh.used = 1;
    fresh.key = *key;
    fresh.stored_ms = now_ms;
    fresh.count = count;
    if (key->want_rows)
    {
        for (i = 0; i < 4; i++)
        {
            size_t n = (size_t)(count * key->sizes[i]);
            if (!n) continue;
            if (!data[i] || !(fresh.rows[i] = malloc( n )))
            {
                free_entry( &fresh );
                if (e) free_entry( e );
                return;
            }
            memcpy( fresh.rows[i], data[i], n );
        }
    }

    if (!e)
    {
        for (i = 0; i < IOS_NSI_CACHE_SLOTS; i++)
            if (!c->slot[i].used) { e = &c->slot[i]; break; }
    }
    if (!e)
    {
        e = &c->slot[0];
        for (i = 1; i < IOS_NSI_CACHE_SLOTS; i++)
            if (c->slot[i].stored_ms < e->stored_ms) e = &c->slot[i];
    }
    free_entry( e );
    *e = fresh;
}

/* ---------------------------------------------------------------- environment */

int ios_nsi_env_on( const char *value )
{
    return !(value && !strcmp( value, "0" ));
}

/* ---------------------------------------------------------------- I6: routing messages */

size_t ios_route_msg_len( const unsigned char *p )
{
    uint16_t len;

    memcpy( &len, p, sizeof(len) );
    return len;
}

const unsigned char *ios_route_msg_at( const unsigned char *p, const unsigned char *end,
                                       size_t hdr_size )
{
    size_t len;

    if (!p || p >= end || (size_t)(end - p) < hdr_size) return NULL;
    len = ios_route_msg_len( p );
    if (len < hdr_size || len > (size_t)(end - p)) return NULL;
    return p;
}
