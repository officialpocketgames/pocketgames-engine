// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part I: the pure logic of nsi.dll's unix-call table (nsi/nsi_unix.c, nsi/nsi_tcp.c),
// kept free of Wine headers so the host tests can drive it:
//   - the 32-bit layouts of the three argument structs (I1);
//   - the dispatcher's parameter checks (I3);
//   - the TCP tables' state filters and the outcome of the server request (I2);
//   - the short cache of enumerations and its environment value (I4);
//   - a bounded walk over routing-socket messages (I6).
// Status values are NTSTATUS numbers.

#ifndef MYIOSDECK_IOS_NSI_PURE_H
#define MYIOSDECK_IOS_NSI_PURE_H

#include <stddef.h>
#include <stdint.h>

#define IOS_NSI_STATUS_SUCCESS           0x00000000u
#define IOS_NSI_STATUS_BUFFER_OVERFLOW   0x80000005u
#define IOS_NSI_STATUS_INVALID_PARAMETER 0xc000000du
#define IOS_NSI_STATUS_NO_MEMORY         0xc0000017u
#define IOS_NSI_STATUS_BUFFER_TOO_SMALL  0xc0000023u
#define IOS_NSI_STATUS_NOT_SUPPORTED     0xc00000bbu

/* ---------------------------------------------------------------- I1: 32-bit layouts */
/* struct nsi_enumerate_all_ex / nsi_get_all_parameters_ex / nsi_get_parameter_ex of
 * include/wine/nsi.h as an i386 compiler lays them out: pointers and UINT_PTR are 4 bytes,
 * every pointer is a guest address */

struct ios_nsi_enumerate_all_ex32
{
    uint32_t unknown[2];
    uint32_t module;          /* const NPI_MODULEID * */
    uint32_t table;           /* UINT_PTR */
    uint32_t first_arg;
    uint32_t second_arg;
    uint32_t key_data;
    uint32_t key_size;
    uint32_t rw_data;
    uint32_t rw_size;
    uint32_t dynamic_data;
    uint32_t dynamic_size;
    uint32_t static_data;
    uint32_t static_size;
    uint32_t count;           /* UINT_PTR, in/out */
};

struct ios_nsi_get_all_parameters_ex32
{
    uint32_t unknown[2];
    uint32_t module;
    uint32_t table;
    uint32_t first_arg;
    uint32_t unknown2;
    uint32_t key;
    uint32_t key_size;
    uint32_t rw_data;
    uint32_t rw_size;
    uint32_t dynamic_data;
    uint32_t dynamic_size;
    uint32_t static_data;
    uint32_t static_size;
};

struct ios_nsi_get_parameter_ex32
{
    uint32_t unknown[2];
    uint32_t module;
    uint32_t table;
    uint32_t first_arg;
    uint32_t unknown2;
    uint32_t key;
    uint32_t key_size;
    uint32_t param_type;      /* UINT_PTR */
    uint32_t data;
    uint32_t data_size;
    uint32_t data_offset;
};

_Static_assert( sizeof(struct ios_nsi_enumerate_all_ex32) == 60, "enumerate_all_ex32 is 60 bytes" );
_Static_assert( offsetof(struct ios_nsi_enumerate_all_ex32, module) == 8, "module at 8" );
_Static_assert( offsetof(struct ios_nsi_enumerate_all_ex32, key_data) == 24, "key_data at 24" );
_Static_assert( offsetof(struct ios_nsi_enumerate_all_ex32, count) == 56, "count at 56" );
_Static_assert( sizeof(struct ios_nsi_get_all_parameters_ex32) == 56, "get_all_parameters_ex32 is 56 bytes" );
_Static_assert( offsetof(struct ios_nsi_get_all_parameters_ex32, key) == 24, "key at 24" );
_Static_assert( offsetof(struct ios_nsi_get_all_parameters_ex32, static_size) == 52, "static_size at 52" );
_Static_assert( sizeof(struct ios_nsi_get_parameter_ex32) == 48, "get_parameter_ex32 is 48 bytes" );
_Static_assert( offsetof(struct ios_nsi_get_parameter_ex32, param_type) == 32, "param_type at 32" );
_Static_assert( offsetof(struct ios_nsi_get_parameter_ex32, data_offset) == 44, "data_offset at 44" );

/* ---------------------------------------------------------------- I3: parameter checks */
/* table_sizes: the table's key, rw, dynamic and static row sizes. Every check returns
 * IOS_NSI_STATUS_SUCCESS or IOS_NSI_STATUS_INVALID_PARAMETER. */

/* enumerate: each of the four parts is 0 (its buffer is then ignored, out[i] = NULL) or
 * exactly the table's size with a buffer (out[i] = data[i]) */
extern uint32_t ios_nsi_check_enumerate( const uint32_t table_sizes[4], const uint32_t sizes[4],
                                         void *const data[4], void *out[4] );

/* one row: the key size is the table's, a non-empty key has a buffer; rw, dynamic, static as
 * in an enumerate (sizes[0], data[0] are the key's) */
extern uint32_t ios_nsi_check_get_all( const uint32_t table_sizes[4], const uint32_t sizes[4],
                                       void *const data[4], void *out[4] );

/* one field: param_type 0 (rw), 1 (dynamic) or 2 (static); the key size is the table's, a
 * non-empty key or field has a buffer, offset and offset + size stay inside the row */
extern uint32_t ios_nsi_check_get_parameter( const uint32_t table_sizes[4], uint32_t key_size,
                                             const void *key, uint64_t param_type,
                                             const void *data, uint32_t data_size,
                                             uint32_t data_offset );

/* ---------------------------------------------------------------- I2: TCP tables */
#define IOS_NSI_TCP_ALL_TABLE     3   /* NSI_TCP_ALL_TABLE */
#define IOS_NSI_TCP_ESTAB_TABLE   4   /* NSI_TCP_ESTAB_TABLE */
#define IOS_NSI_TCP_LISTEN_TABLE  5   /* NSI_TCP_LISTEN_TABLE */
#define IOS_MIB_TCP_STATE_LISTEN  2
#define IOS_MIB_TCP_STATE_ESTAB   5

/* the server's state filter for a TCP table, or -1 for a table not served */
extern int ios_nsi_tcp_state_filter( uint32_t table );

/* what a TCP enumerate answers after the server's get_tcp_connections request: want_data is
 * whether any row buffer was given; *count is the caller's room in, the number of rows to fill
 * out (untouched on overflow). With no buffers the call returns the count only; with buffers
 * too small for the connections, STATUS_BUFFER_OVERFLOW. */
extern uint32_t ios_nsi_tcp_outcome( int want_data, uint32_t server_status, uint32_t server_count,
                                     uint64_t *count );

/* ---------------------------------------------------------------- I4: the cache */
#define IOS_NSI_CACHE_SLOTS      8
#define IOS_NSI_CACHE_MAX_BYTES  (1024u * 1024u)
#define IOS_NSI_CACHE_DEFAULT_MS 500u
#define IOS_NSI_CACHE_MAX_MS     5000u

struct ios_nsi_cache_key
{
    uint32_t module;          /* the dispatcher's module index */
    uint32_t table;
    uint32_t first_arg;
    uint32_t second_arg;
    uint32_t sizes[4];        /* key, rw, dynamic, static */
    uint32_t want_rows;       /* rows asked for, or only the count */
};

struct ios_nsi_cache_entry
{
    int used;
    struct ios_nsi_cache_key key;
    uint64_t stored_ms;
    uint64_t count;
    unsigned char *rows[4];   /* count * sizes[i] bytes each, malloc'ed (NULL for size 0) */
};

struct ios_nsi_cache
{
    struct ios_nsi_cache_entry slot[IOS_NSI_CACHE_SLOTS];
};

/* PG_NSI_CACHE_MS -> the time to live in ms: unset, empty or not a number gives 500,
 * 0 turns the cache off, anything above 5000 is 5000 */
extern uint32_t ios_nsi_cache_ttl_from_env( const char *value );

/* An identical request stored less than ttl_ms ago answers with the provider's semantics:
 * count only -> *count, SUCCESS; rows -> all rows and *count when *count (the room) is enough,
 * else the rows that fit and BUFFER_OVERFLOW with *count untouched. Returns 1 on a hit
 * (*status set, rows copied into data[]), 0 on a miss. */
extern int ios_nsi_cache_lookup( struct ios_nsi_cache *c, const struct ios_nsi_cache_key *key,
                                 uint64_t now_ms, uint32_t ttl_ms, void *const data[4],
                                 uint64_t *count, uint32_t *status );

/* Remember a successful enumerate (count rows in data[]). Nothing is stored when ttl_ms is 0
 * or the rows are over IOS_NSI_CACHE_MAX_BYTES (an older result of the same request is then
 * dropped). The slot is the same request's, else a free one, else the oldest. */
extern void ios_nsi_cache_store( struct ios_nsi_cache *c, const struct ios_nsi_cache_key *key,
                                 uint64_t now_ms, uint32_t ttl_ms, void *const data[4],
                                 uint64_t count );

/* the slot holding key, or -1 (tests) */
extern int ios_nsi_cache_slot_of( const struct ios_nsi_cache *c, const struct ios_nsi_cache_key *key );

/* ---------------------------------------------------------------- environment */
/* "0" turns a PG_NSI_* switch off; anything else (or unset) leaves it on */
extern int ios_nsi_env_on( const char *value );

/* ---------------------------------------------------------------- I6: routing messages */
/* p if a whole message of at least hdr_size bytes starts at p before end (its length is the
 * 16-bit field at offset 0, as in if_msghdr and rt_msghdr), else NULL: fewer bytes than a
 * header remain, or the length is smaller than the header or runs past the end */
extern const unsigned char *ios_route_msg_at( const unsigned char *p, const unsigned char *end,
                                              size_t hdr_size );
/* the length of the message at p (checked by ios_route_msg_at) */
extern size_t ios_route_msg_len( const unsigned char *p );

#endif /* MYIOSDECK_IOS_NSI_PURE_H */
