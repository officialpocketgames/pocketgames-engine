// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part K: the pure parts of the NSI table (common/ios_nsi_pure.c): the dispatcher's
// checks (sizes, NULL buffers, offset overflow, param_type), the TCP filter and size rules, the
// cache (hit within the TTL, miss after, a different size misses, overflow keeps the count,
// failures and tables over 1 MB not cached, eviction of the oldest of 8, the env value parsed
// and capped), the 32-bit layouts (I1, also _Static_assert'ed in the header) and the routing
// structs (I5, include/net/route.h), and the bounded message walk (I6).

#include <stdlib.h>
#include <string.h>

#include "ios_nsi_pure.h"
#include "net/route.h"
#include "ios_test.h"

#define OK   IOS_NSI_STATUS_SUCCESS
#define INV  IOS_NSI_STATUS_INVALID_PARAMETER
#define OVF  IOS_NSI_STATUS_BUFFER_OVERFLOW

static void test_checks( void )
{
    const uint32_t table[4] = { 8, 0, 24, 40 };   /* key, rw (none), dynamic, static */
    char key[8 * 4], dyn[24 * 4], stat[40 * 4];
    void *out[4];

    /* enumerate: 0 or exactly the table's size; a non-zero size needs a buffer */
    {
        uint32_t sizes[4] = { 8, 0, 24, 40 };
        void *data[4] = { key, NULL, dyn, stat };
        CHECK_EQ( ios_nsi_check_enumerate( table, sizes, data, out ), OK );
        CHECK( out[0] == key && out[1] == NULL && out[2] == dyn && out[3] == stat );
    }
    {
        uint32_t sizes[4] = { 0, 0, 0, 0 };                    /* count only */
        void *data[4] = { key, key, key, key };                /* ignored */
        CHECK_EQ( ios_nsi_check_enumerate( table, sizes, data, out ), OK );
        CHECK( !out[0] && !out[1] && !out[2] && !out[3] );
    }
    {
        uint32_t sizes[4] = { 8, 0, 0, 40 };                   /* some parts only */
        void *data[4] = { key, NULL, NULL, stat };
        CHECK_EQ( ios_nsi_check_enumerate( table, sizes, data, out ), OK );
    }
    {
        uint32_t sizes[4] = { 8, 0, 23, 40 };                  /* a size that isn't the table's */
        void *data[4] = { key, NULL, dyn, stat };
        CHECK_EQ( ios_nsi_check_enumerate( table, sizes, data, out ), INV );
    }
    {
        uint32_t sizes[4] = { 8, 16, 24, 40 };                 /* rw size on a table with none */
        void *data[4] = { key, dyn, dyn, stat };
        CHECK_EQ( ios_nsi_check_enumerate( table, sizes, data, out ), INV );
    }
    {
        uint32_t sizes[4] = { 8, 0, 24, 40 };                  /* non-zero size, NULL buffer */
        void *data[4] = { key, NULL, NULL, stat };
        CHECK_EQ( ios_nsi_check_enumerate( table, sizes, data, out ), INV );
    }

    /* one row: the key size is always the table's */
    {
        uint32_t sizes[4] = { 8, 0, 24, 0 };
        void *data[4] = { key, NULL, dyn, NULL };
        CHECK_EQ( ios_nsi_check_get_all( table, sizes, data, out ), OK );
        CHECK( out[0] == key && out[2] == dyn && !out[3] );
    }
    {
        uint32_t sizes[4] = { 4, 0, 24, 0 };
        void *data[4] = { key, NULL, dyn, NULL };
        CHECK_EQ( ios_nsi_check_get_all( table, sizes, data, out ), INV );
    }
    {
        uint32_t sizes[4] = { 8, 0, 24, 0 };                   /* key size without a key */
        void *data[4] = { NULL, NULL, dyn, NULL };
        CHECK_EQ( ios_nsi_check_get_all( table, sizes, data, out ), INV );
    }
    {
        uint32_t sizes[4] = { 8, 0, 0, 40 };                   /* static size, NULL buffer */
        void *data[4] = { key, NULL, NULL, NULL };
        CHECK_EQ( ios_nsi_check_get_all( table, sizes, data, out ), INV );
    }

    /* one field */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, stat, 4, 0 ), OK );
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, stat, 4, 36 ), OK );     /* last field */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, stat, 40, 0 ), OK );     /* whole row */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, stat, 4, 37 ), INV );    /* runs past */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, stat, 0, 41 ), INV );    /* offset past */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, stat, 8, 0xfffffffcu ), INV ); /* wraps */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, stat, 0xffffffffu, 8 ), INV );
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 1, dyn, 4, 20 ), OK );      /* dynamic row 24 */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 1, dyn, 4, 24 ), INV );
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 0, dyn, 4, 0 ), INV );      /* rw row is 0 */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 3, stat, 4, 0 ), INV );     /* param_type > 2 */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 0x100000002ull, stat, 4, 0 ), INV );
    CHECK_EQ( ios_nsi_check_get_parameter( table, 4, key, 2, stat, 4, 0 ), INV );     /* key size */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, NULL, 2, stat, 4, 0 ), INV );    /* NULL key */
    CHECK_EQ( ios_nsi_check_get_parameter( table, 8, key, 2, NULL, 4, 0 ), INV );     /* NULL data */
}

static void test_tcp( void )
{
    const uint32_t tcp_table[4] = { 56, 0, 20, 24 };
    char key[56], dyn[20], stat[24];
    void *out[4];
    uint64_t count;

    /* filters: ALL 0, ESTAB MIB_TCP_STATE_ESTAB, LISTEN MIB_TCP_STATE_LISTEN, others not served */
    CHECK_EQ( ios_nsi_tcp_state_filter( IOS_NSI_TCP_ALL_TABLE ), 0 );
    CHECK_EQ( ios_nsi_tcp_state_filter( IOS_NSI_TCP_ESTAB_TABLE ), 5 );
    CHECK_EQ( ios_nsi_tcp_state_filter( IOS_NSI_TCP_LISTEN_TABLE ), 2 );
    CHECK_EQ( ios_nsi_tcp_state_filter( 0 ), (unsigned long long)-1 );   /* NSI_TCP_STATS_TABLE */
    CHECK_EQ( ios_nsi_tcp_state_filter( 6 ), (unsigned long long)-1 );

    /* sizes: key, dynamic, static 0 or exactly their struct's size; rw must be 0 */
    {
        uint32_t sizes[4] = { 56, 0, 20, 24 };
        void *data[4] = { key, NULL, dyn, stat };
        CHECK_EQ( ios_nsi_check_enumerate( tcp_table, sizes, data, out ), OK );
    }
    {
        uint32_t sizes[4] = { 56, 4, 20, 24 };
        void *data[4] = { key, dyn, dyn, stat };
        CHECK_EQ( ios_nsi_check_enumerate( tcp_table, sizes, data, out ), INV );
    }
    {
        uint32_t sizes[4] = { 0, 0, 0, 24 };                   /* static rows only (owner pids) */
        void *data[4] = { NULL, NULL, NULL, stat };
        CHECK_EQ( ios_nsi_check_enumerate( tcp_table, sizes, data, out ), OK );
    }
    {
        uint32_t sizes[4] = { 56, 0, 20, 28 };
        void *data[4] = { key, NULL, dyn, stat };
        CHECK_EQ( ios_nsi_check_enumerate( tcp_table, sizes, data, out ), INV );
    }

    /* outcome: no buffers -> the count only; buffers too small -> overflow, count untouched */
    count = 0;
    CHECK_EQ( ios_nsi_tcp_outcome( 0, IOS_NSI_STATUS_BUFFER_TOO_SMALL, 7, &count ), OK );
    CHECK_EQ( count, 7 );
    count = 0;
    CHECK_EQ( ios_nsi_tcp_outcome( 0, OK, 0, &count ), OK );
    CHECK_EQ( count, 0 );
    count = 3;
    CHECK_EQ( ios_nsi_tcp_outcome( 1, IOS_NSI_STATUS_BUFFER_TOO_SMALL, 7, &count ), OVF );
    CHECK_EQ( count, 3 );
    count = 10;
    CHECK_EQ( ios_nsi_tcp_outcome( 1, OK, 7, &count ), OK );
    CHECK_EQ( count, 7 );
    count = 10;
    CHECK_EQ( ios_nsi_tcp_outcome( 1, 0xc0000001u, 7, &count ), 0xc0000001u );   /* other errors pass */
    CHECK_EQ( count, 10 );
}

static struct ios_nsi_cache cache;

static struct ios_nsi_cache_key make_key( uint32_t module, uint32_t table, uint32_t rows )
{
    struct ios_nsi_cache_key k;

    memset( &k, 0, sizeof(k) );
    k.module = module;
    k.table = table;
    k.first_arg = 1;
    if (rows)
    {
        k.sizes[0] = 8;
        k.sizes[3] = 16;
        k.want_rows = 1;
    }
    return k;
}

static void test_cache( void )
{
    unsigned char key_rows[8 * 3], stat_rows[16 * 3], out_key[8 * 3], out_stat[16 * 3];
    void *data[4] = { key_rows, NULL, NULL, stat_rows };
    void *out[4] = { out_key, NULL, NULL, out_stat };
    struct ios_nsi_cache_key k = make_key( 0, 0, 1 ), kc = make_key( 0, 0, 0 ), k2;
    uint64_t count;
    uint32_t status;
    int i;

    memset( &cache, 0, sizeof(cache) );
    for (i = 0; i < (int)sizeof(key_rows); i++) key_rows[i] = (unsigned char)i;
    for (i = 0; i < (int)sizeof(stat_rows); i++) stat_rows[i] = (unsigned char)(100 + i);

    /* the environment value: default 500, 0 off, capped at 5000, junk is the default */
    CHECK_EQ( ios_nsi_cache_ttl_from_env( NULL ), 500 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "" ), 500 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "0" ), 0 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "250" ), 250 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "5000" ), 5000 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "5001" ), 5000 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "99999999999" ), 5000 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "abc" ), 500 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "12ms" ), 500 );
    CHECK_EQ( ios_nsi_cache_ttl_from_env( "-1" ), 500 );
    CHECK( ios_nsi_env_on( NULL ) && ios_nsi_env_on( "1" ) && !ios_nsi_env_on( "0" ) );

    /* nothing cached yet */
    count = 3;
    CHECK( !ios_nsi_cache_lookup( &cache, &k, 1000, 500, out, &count, &status ) );

    /* a successful enumerate of 3 rows, then a hit within the TTL: all rows and the count */
    ios_nsi_cache_store( &cache, &k, 1000, 500, data, 3 );
    memset( out_key, 0, sizeof(out_key) );
    memset( out_stat, 0, sizeof(out_stat) );
    count = 5;
    CHECK( ios_nsi_cache_lookup( &cache, &k, 1499, 500, out, &count, &status ) );
    CHECK_EQ( status, OK );
    CHECK_EQ( count, 3 );
    CHECK( !memcmp( out_key, key_rows, sizeof(key_rows) ) );
    CHECK( !memcmp( out_stat, stat_rows, sizeof(stat_rows) ) );

    /* the caller's room smaller: the rows that fit, overflow, count untouched */
    memset( out_key, 0, sizeof(out_key) );
    count = 2;
    CHECK( ios_nsi_cache_lookup( &cache, &k, 1100, 500, out, &count, &status ) );
    CHECK_EQ( status, OVF );
    CHECK_EQ( count, 2 );
    CHECK( !memcmp( out_key, key_rows, 16 ) );
    CHECK_EQ( out_key[16], 0 );

    /* a miss after the TTL */
    count = 5;
    CHECK( !ios_nsi_cache_lookup( &cache, &k, 1500, 500, out, &count, &status ) );
    CHECK_EQ( count, 5 );

    /* a different size, table, argument or "count only" misses */
    k2 = k; k2.sizes[3] = 24;
    CHECK( !ios_nsi_cache_lookup( &cache, &k2, 1100, 500, out, &count, &status ) );
    k2 = k; k2.table = 1;
    CHECK( !ios_nsi_cache_lookup( &cache, &k2, 1100, 500, out, &count, &status ) );
    k2 = k; k2.second_arg = 9;
    CHECK( !ios_nsi_cache_lookup( &cache, &k2, 1100, 500, out, &count, &status ) );
    k2 = k; k2.module = 1;
    CHECK( !ios_nsi_cache_lookup( &cache, &k2, 1100, 500, out, &count, &status ) );
    CHECK( !ios_nsi_cache_lookup( &cache, &kc, 1100, 500, out, &count, &status ) );

    /* count only */
    ios_nsi_cache_store( &cache, &kc, 2000, 500, data, 42 );
    count = 0;
    CHECK( ios_nsi_cache_lookup( &cache, &kc, 2001, 500, out, &count, &status ) );
    CHECK_EQ( status, OK );
    CHECK_EQ( count, 42 );

    /* TTL 0: nothing stored or found */
    memset( &cache, 0, sizeof(cache) );
    ios_nsi_cache_store( &cache, &k, 1000, 0, data, 3 );
    CHECK_EQ( ios_nsi_cache_slot_of( &cache, &k ), (unsigned long long)-1 );
    CHECK( !ios_nsi_cache_lookup( &cache, &k, 1000, 0, out, &count, &status ) );

    /* failures are never cached: the dispatcher stores only successes, so nothing to look up */
    count = 3;
    CHECK( !ios_nsi_cache_lookup( &cache, &k, 1001, 500, out, &count, &status ) );

    /* a table over 1 MB is not cached, and drops an older result of the same request */
    {
        struct ios_nsi_cache_key big = make_key( 1, 7, 1 );
        uint64_t n = IOS_NSI_CACHE_MAX_BYTES / 24 + 1;     /* 8 + 16 bytes per row */
        unsigned char *bk = calloc( n, 8 ), *bs = calloc( n, 16 );
        void *bdata[4] = { bk, NULL, NULL, bs };

        ios_nsi_cache_store( &cache, &big, 3000, 500, data, 3 );
        CHECK( ios_nsi_cache_slot_of( &cache, &big ) >= 0 );
        ios_nsi_cache_store( &cache, &big, 3001, 500, bdata, n );
        CHECK_EQ( ios_nsi_cache_slot_of( &cache, &big ), (unsigned long long)-1 );
        ios_nsi_cache_store( &cache, &big, 3002, 500, bdata, n - 2 );   /* just under 1 MB: kept */
        CHECK( ios_nsi_cache_slot_of( &cache, &big ) >= 0 );
        free( bk );
        free( bs );
    }

    /* at most 8: the same request reuses its slot, a ninth evicts the oldest */
    memset( &cache, 0, sizeof(cache) );
    for (i = 0; i < IOS_NSI_CACHE_SLOTS; i++)
    {
        k2 = make_key( 1, 100 + i, 1 );
        ios_nsi_cache_store( &cache, &k2, 5000 + i, 500, data, 1 );
    }
    k2 = make_key( 1, 103, 1 );
    i = ios_nsi_cache_slot_of( &cache, &k2 );
    ios_nsi_cache_store( &cache, &k2, 5100, 500, data, 2 );            /* same request: its slot */
    CHECK_EQ( ios_nsi_cache_slot_of( &cache, &k2 ), i );
    count = 5;
    CHECK( ios_nsi_cache_lookup( &cache, &k2, 5101, 500, out, &count, &status ) );
    CHECK_EQ( count, 2 );
    k2 = make_key( 1, 200, 1 );
    ios_nsi_cache_store( &cache, &k2, 5200, 500, data, 1 );            /* ninth request */
    CHECK( ios_nsi_cache_slot_of( &cache, &k2 ) >= 0 );
    k2 = make_key( 1, 100, 1 );                                        /* the oldest: gone */
    CHECK_EQ( ios_nsi_cache_slot_of( &cache, &k2 ), (unsigned long long)-1 );
    for (i = 1; i < IOS_NSI_CACHE_SLOTS; i++)
    {
        k2 = make_key( 1, 100 + i, 1 );
        CHECK( ios_nsi_cache_slot_of( &cache, &k2 ) >= 0 );
    }

    for (i = 0; i < IOS_NSI_CACHE_SLOTS; i++)
    {
        free( cache.slot[i].rows[0] ); free( cache.slot[i].rows[1] );
        free( cache.slot[i].rows[2] ); free( cache.slot[i].rows[3] );
    }
}

static void test_layouts( void )
{
    /* I1: the i386 layouts (the header _Static_asserts them too) */
    CHECK_EQ( sizeof(struct ios_nsi_enumerate_all_ex32), 60 );
    CHECK_EQ( sizeof(struct ios_nsi_get_all_parameters_ex32), 56 );
    CHECK_EQ( sizeof(struct ios_nsi_get_parameter_ex32), 48 );
    CHECK_EQ( offsetof(struct ios_nsi_enumerate_all_ex32, table), 12 );
    CHECK_EQ( offsetof(struct ios_nsi_enumerate_all_ex32, static_size), 52 );
    CHECK_EQ( offsetof(struct ios_nsi_get_parameter_ex32, data), 36 );

    /* I5: Darwin's routing-message ABI */
    CHECK_EQ( sizeof(struct rt_metrics), 56 );
    CHECK_EQ( sizeof(struct rt_msghdr), 92 );
    CHECK_EQ( offsetof(struct rt_msghdr, rtm_flags), 8 );
    CHECK_EQ( offsetof(struct rt_msghdr, rtm_rmx), 36 );
    CHECK_EQ( offsetof(struct rt_msghdr, rtm_rmx) + offsetof(struct rt_metrics, rmx_hopcount), 44 );
    CHECK_EQ( sizeof(struct sockaddr_inarp), 16 );
    CHECK_EQ( RTM_GET, 0x4 );
    CHECK_EQ( RTM_IFINFO, 0xe );
    CHECK_EQ( RTF_GATEWAY | RTF_LLINFO | RTF_MULTICAST, 0x800402 );
    CHECK_EQ( RTA_DST | RTA_GATEWAY | RTA_NETMASK | RTA_IFP, 0x17 );
}

static void put16( unsigned char *p, uint16_t v ) { memcpy( p, &v, 2 ); }

static void test_walk( void )
{
    unsigned char buf[64];
    const unsigned char *p, *end = buf + sizeof(buf);
    int n;

    /* three messages of 16, 24 and 24 bytes with a 12-byte header: all three visited */
    memset( buf, 0, sizeof(buf) );
    put16( buf, 16 ); put16( buf + 16, 24 ); put16( buf + 40, 24 );
    for (n = 0, p = buf; (p = ios_route_msg_at( p, end, 12 )); p += ios_route_msg_len( p )) n++;
    CHECK_EQ( n, 3 );

    /* a zero length (upstream looped forever) stops the walk */
    put16( buf + 16, 0 );
    for (n = 0, p = buf; (p = ios_route_msg_at( p, end, 12 )); p += ios_route_msg_len( p )) n++;
    CHECK_EQ( n, 1 );

    /* a length smaller than the header stops it */
    put16( buf + 16, 8 );
    for (n = 0, p = buf; (p = ios_route_msg_at( p, end, 12 )); p += ios_route_msg_len( p )) n++;
    CHECK_EQ( n, 1 );

    /* a length running past the end stops it */
    put16( buf + 16, 24 ); put16( buf + 40, 25 );
    for (n = 0, p = buf; (p = ios_route_msg_at( p, end, 12 )); p += ios_route_msg_len( p )) n++;
    CHECK_EQ( n, 2 );

    /* fewer bytes than a header remain */
    CHECK( ios_route_msg_at( end - 11, end, 12 ) == NULL );
    CHECK( ios_route_msg_at( end, end, 12 ) == NULL );
    CHECK( ios_route_msg_at( NULL, end, 12 ) == NULL );
}

int main( void )
{
    test_checks();
    test_tcp();
    test_cache();
    test_layouts();
    test_walk();
    return ios_test_done( "test_nsi" );
}
