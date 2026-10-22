// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 8.3: PKCS #1 DER and ECDSA signature formats. See include/ios_der.h.

#include <stdlib.h>
#include <string.h>

#include "ios_der.h"

struct ios_be ios_be_trim( const unsigned char *data, size_t len )
{
    struct ios_be b;

    while (len && !*data) { data++; len--; }
    b.data = data;
    b.len = len;
    return b;
}

/* ---------------------------------------------------------------- writing */

static size_t len_size( size_t len )
{
    if (len < 0x80) return 1;
    if (len <= 0xff) return 2;
    if (len <= 0xffff) return 3;
    if (len <= 0xffffff) return 4;
    return 5;
}

static unsigned char *put_len( unsigned char *p, size_t len )
{
    size_t n = len_size( len ) - 1, i;

    if (!n) { *p++ = (unsigned char)len; return p; }
    *p++ = 0x80 | (unsigned char)n;
    for (i = n; i > 0; i--) *p++ = (unsigned char)(len >> ((i - 1) * 8));
    return p;
}

/* the content length of an INTEGER holding the unsigned value v */
static size_t int_content( struct ios_be v )
{
    v = ios_be_trim( v.data, v.len );
    if (!v.len) return 1;                     /* zero is one 00 byte */
    return v.len + ((v.data[0] & 0x80) ? 1 : 0);
}

static unsigned char *put_int( unsigned char *p, struct ios_be v )
{
    size_t content = int_content( v );

    v = ios_be_trim( v.data, v.len );
    *p++ = 0x02;
    p = put_len( p, content );
    if (content > v.len) *p++ = 0;
    if (v.len) memcpy( p, v.data, v.len );
    return p + v.len;
}

static int encode_ints( const struct ios_be *ints, size_t count, unsigned char **der, size_t *len )
{
    size_t body = 0, i, c;
    unsigned char *buf, *p;

    for (i = 0; i < count; i++)
    {
        c = int_content( ints[i] );
        body += 1 + len_size( c ) + c;
    }
    *len = 1 + len_size( body ) + body;
    if (!(buf = malloc( *len ))) return -1;
    p = buf;
    *p++ = 0x30;
    p = put_len( p, body );
    for (i = 0; i < count; i++) p = put_int( p, ints[i] );
    *der = buf;
    return 0;
}

int ios_der_rsa_private( const struct ios_rsa_parts *k, unsigned char **der, size_t *len )
{
    static const unsigned char zero = 0;
    struct ios_be ints[9];

    if (!k->n.len || !k->e.len || !k->d.len || !k->p.len || !k->q.len ||
        !k->dp.len || !k->dq.len || !k->qinv.len) return -1;
    ints[0].data = &zero; ints[0].len = 1;   /* version 0 (two-prime) */
    ints[1] = k->n; ints[2] = k->e; ints[3] = k->d; ints[4] = k->p; ints[5] = k->q;
    ints[6] = k->dp; ints[7] = k->dq; ints[8] = k->qinv;
    return encode_ints( ints, 9, der, len );
}

int ios_der_rsa_public( const struct ios_rsa_parts *k, unsigned char **der, size_t *len )
{
    struct ios_be ints[2];

    if (!k->n.len || !k->e.len) return -1;
    ints[0] = k->n;
    ints[1] = k->e;
    return encode_ints( ints, 2, der, len );
}

/* ---------------------------------------------------------------- reading */

/* read a tag and definite length; *p advances past the header; returns the content length or -1 */
static long get_header( const unsigned char **p, const unsigned char *end, unsigned char tag )
{
    size_t len = 0, n;

    if (end - *p < 2 || **p != tag) return -1;
    (*p)++;
    if (!(**p & 0x80)) len = *(*p)++;
    else
    {
        n = *(*p)++ & 0x7f;
        if (!n || n > 4 || (size_t)(end - *p) < n) return -1;
        while (n--) len = (len << 8) | *(*p)++;
    }
    if (len > (size_t)(end - *p)) return -1;
    return (long)len;
}

static int get_int( const unsigned char **p, const unsigned char *end, struct ios_be *v )
{
    long len = get_header( p, end, 0x02 );

    if (len <= 0) return -1;
    if (**p & 0x80) return -1;                /* negative: not a key or signature value */
    *v = ios_be_trim( *p, (size_t)len );
    *p += len;
    return 0;
}

int ios_der_rsa_parse( const unsigned char *der, size_t len, struct ios_rsa_parts *k, int *is_private )
{
    const unsigned char *p = der, *end = der + len;
    struct ios_be v[9];
    long body;
    int count = 0;

    memset( k, 0, sizeof(*k) );
    if ((body = get_header( &p, end, 0x30 )) < 0) return -1;
    end = p + body;
    while (p < end && count < 9)
        if (get_int( &p, end, &v[count++] )) return -1;
    if (p != end) return -1;

    if (count == 2)
    {
        k->n = v[0]; k->e = v[1];
        *is_private = 0;
        return k->n.len && k->e.len ? 0 : -1;
    }
    if (count == 9 && !v[0].len)              /* version 0 */
    {
        k->n = v[1]; k->e = v[2]; k->d = v[3]; k->p = v[4]; k->q = v[5];
        k->dp = v[6]; k->dq = v[7]; k->qinv = v[8];
        *is_private = 1;
        return k->n.len && k->e.len ? 0 : -1;
    }
    return -1;
}

int ios_der_sig_to_rs( const unsigned char *der, size_t len, unsigned char *out, size_t half )
{
    const unsigned char *p = der, *end = der + len;
    struct ios_be r, s;
    long body;

    if ((body = get_header( &p, end, 0x30 )) < 0) return -1;
    end = p + body;
    if (get_int( &p, end, &r ) || get_int( &p, end, &s ) || p != end) return -1;
    if (r.len > half || s.len > half) return -1;
    memset( out, 0, 2 * half );
    memcpy( out + half - r.len, r.data, r.len );
    memcpy( out + 2 * half - s.len, s.data, s.len );
    return 0;
}
