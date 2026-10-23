// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarifications 8.4, 8.7, 13: LibTomCrypt 1.18.2 / LibTomMath (libtomcrypt_unix.a) for
// bcrypt's DH, DSA and the RSAPRIVATE completion, behind include/ios_ltc.h. Only routing and
// format conversion: the arithmetic is LibTomCrypt's.
//
// Also here, because Wine's copy of LibTomCrypt lacks it: hash_memory (dsa_generate_pqg calls
// it), and the PRNG descriptor whose read is SecRandomCopyBytes (clarification 8.7).
//
// Compiled with Wine's LibTomCrypt defines (-DLTC_NO_PROTOTYPES -DLTC_SOURCE -DLTC_NO_TEST
// -DMP_32BIT=) and include/ios_tomcrypt_rename.h first.

#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "ios_tomcrypt_rename.h"
#include <tomcrypt.h>

#include "ios_applecrypto.h"
#include "ios_ltc.h"

/* ---------------------------------------------------------------- hash_memory */

int hash_memory( int hash, const unsigned char *in, unsigned long inlen,
                 unsigned char *out, unsigned long *outlen )
{
    hash_state md;
    int err;

    if ((err = hash_is_valid( hash )) != CRYPT_OK) return err;
    if (*outlen < hash_descriptor[hash].hashsize)
    {
        *outlen = hash_descriptor[hash].hashsize;
        return CRYPT_BUFFER_OVERFLOW;
    }
    if ((err = hash_descriptor[hash].init( &md )) != CRYPT_OK) return err;
    if ((err = hash_descriptor[hash].process( &md, in, inlen )) != CRYPT_OK) return err;
    if ((err = hash_descriptor[hash].done( &md, out )) != CRYPT_OK) return err;
    *outlen = hash_descriptor[hash].hashsize;
    return CRYPT_OK;
}

/* ---------------------------------------------------------------- the PRNG */

static int prng_start( prng_state *prng ) { (void)prng; return CRYPT_OK; }
static int prng_add_entropy( const unsigned char *in, unsigned long len, prng_state *prng )
{
    (void)in; (void)len; (void)prng;
    return CRYPT_OK;
}
static int prng_ready( prng_state *prng ) { (void)prng; return CRYPT_OK; }
static unsigned long prng_read( unsigned char *out, unsigned long len, prng_state *prng )
{
    (void)prng;
    return ios_ac_random( out, len ) == IOS_AC_OK ? len : 0;
}
static int prng_done( prng_state *prng ) { (void)prng; return CRYPT_OK; }
static int prng_export( unsigned char *out, unsigned long *outlen, prng_state *prng )
{
    (void)out; (void)prng;
    *outlen = 0;
    return CRYPT_OK;
}
static int prng_import( const unsigned char *in, unsigned long len, prng_state *prng )
{
    (void)in; (void)len; (void)prng;
    return CRYPT_OK;
}
static int prng_test( void ) { return CRYPT_OK; }

static const struct ltc_prng_descriptor ios_prng_desc =
{
    .name = "ios-secrandom",
    .export_size = 0,
    .start = prng_start,
    .add_entropy = prng_add_entropy,
    .ready = prng_ready,
    .read = prng_read,
    .done = prng_done,
    .pexport = prng_export,
    .pimport = prng_import,
    .test = prng_test,
};

/* ---------------------------------------------------------------- init (no pthread_once) */

static int init_state;      /* 0 not done, 1 in progress, 2 done, 3 failed */
static int prng_index = -1;

int ios_ltc_init( void )
{
    int expected = 0;

    if (__atomic_compare_exchange_n( &init_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
    {
        int ok;
        ltc_mp = ltm_desc;
        ok = register_hash( &sha1_desc ) >= 0 && register_hash( &sha256_desc ) >= 0 &&
             register_hash( &sha384_desc ) >= 0 && register_hash( &sha512_desc ) >= 0 &&
             (prng_index = register_prng( &ios_prng_desc )) >= 0;
        __atomic_store_n( &init_state, ok ? 2 : 3, __ATOMIC_RELEASE );
    }
    while ((expected = __atomic_load_n( &init_state, __ATOMIC_ACQUIRE )) == 1) sched_yield();
    return expected == 2 ? 0 : -1;
}

/* ---------------------------------------------------------------- big numbers */

void ios_buf_free( struct ios_buf *b )
{
    if (!b) return;
    free( b->data );
    b->data = NULL;
    b->len = 0;
}

/* an mp integer as a minimal big-endian byte string */
static int to_buf( void *a, struct ios_buf *b )
{
    unsigned long n = mp_unsigned_bin_size( a );

    b->len = n;
    if (!(b->data = malloc( n ? n : 1 ))) return -1;
    if (n && mp_to_unsigned_bin( a, b->data ) != CRYPT_OK) { ios_buf_free( b ); return -1; }
    return 0;
}

static int read_mp( void *a, const unsigned char *data, size_t len )
{
    return mp_read_unsigned_bin( a, (unsigned char *)data, (unsigned long)len ) == CRYPT_OK ? 0 : -1;
}

int ios_ltc_rsa_complete( const unsigned char *e, size_t elen, const unsigned char *p, size_t plen,
                          const unsigned char *q, size_t qlen, struct ios_buf *d, struct ios_buf *dp,
                          struct ios_buf *dq, struct ios_buf *qinv )
{
    void *me, *mp, *mq, *p1, *q1, *phi, *md, *mdp, *mdq, *mqi;
    int ret = -1;

    memset( d, 0, sizeof(*d) ); memset( dp, 0, sizeof(*dp) );
    memset( dq, 0, sizeof(*dq) ); memset( qinv, 0, sizeof(*qinv) );
    if (ios_ltc_init()) return -1;
    if (mp_init_multi( &me, &mp, &mq, &p1, &q1, &phi, &md, &mdp, &mdq, &mqi, NULL ) != CRYPT_OK) return -1;
    if (read_mp( me, e, elen ) || read_mp( mp, p, plen ) || read_mp( mq, q, qlen )) goto done;
    if (mp_sub_d( mp, 1, p1 ) != CRYPT_OK || mp_sub_d( mq, 1, q1 ) != CRYPT_OK) goto done;
    if (mp_mul( p1, q1, phi ) != CRYPT_OK) goto done;
    if (mp_invmod( me, phi, md ) != CRYPT_OK) goto done;          /* d = e^-1 mod (p-1)(q-1) */
    if (mp_mod( md, p1, mdp ) != CRYPT_OK) goto done;             /* dp = d mod (p-1) */
    if (mp_mod( md, q1, mdq ) != CRYPT_OK) goto done;             /* dq = d mod (q-1) */
    if (mp_invmod( mq, mp, mqi ) != CRYPT_OK) goto done;          /* qinv = q^-1 mod p */
    if (to_buf( md, d ) || to_buf( mdp, dp ) || to_buf( mdq, dq ) || to_buf( mqi, qinv )) goto done;
    ret = 0;
done:
    if (ret) { ios_buf_free( d ); ios_buf_free( dp ); ios_buf_free( dq ); ios_buf_free( qinv ); }
    mp_clear_multi( me, mp, mq, p1, q1, phi, md, mdp, mdq, mqi, NULL );
    return ret;
}

/* ---------------------------------------------------------------- DH */

int ios_ltc_dh_params( unsigned int bits, int *from_group, struct ios_buf *p, struct ios_buf *g )
{
    static const unsigned int groups[] = { 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192 };
    unsigned int i;
    int ret = -1;

    memset( p, 0, sizeof(*p) );
    memset( g, 0, sizeof(*g) );
    if (ios_ltc_init()) return -1;
    *from_group = 0;
    for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
        if (groups[i] == bits)
        {
            dh_key key;
            memset( &key, 0, sizeof(key) );
            if (dh_set_pg_groupsize( (int)(bits / 8), &key ) != CRYPT_OK) return -1;
            ret = (to_buf( key.prime, p ) || to_buf( key.base, g )) ? -1 : 0;
            dh_free( &key );
            *from_group = 1;
            break;
        }
    if (!*from_group)
    {
        dsa_key key;
        memset( &key, 0, sizeof(key) );
        if (dsa_generate_pqg( NULL, prng_index, 32, (int)(bits / 8), &key ) != CRYPT_OK) return -1;
        ret = (to_buf( key.p, p ) || to_buf( key.g, g )) ? -1 : 0;
        dsa_free( &key );
    }
    if (ret) { ios_buf_free( p ); ios_buf_free( g ); }
    return ret;
}

int ios_ltc_dh_generate( const unsigned char *p, size_t plen, const unsigned char *g, size_t glen,
                         struct ios_buf *y, struct ios_buf *x )
{
    dh_key key;
    int ret;

    memset( y, 0, sizeof(*y) );
    memset( x, 0, sizeof(*x) );
    if (ios_ltc_init()) return -1;
    memset( &key, 0, sizeof(key) );
    if (dh_set_pg( p, plen, g, glen, &key ) != CRYPT_OK) return -1;
    if (dh_generate_key( NULL, prng_index, &key ) != CRYPT_OK) return -1;   /* frees the key on failure */
    ret = (to_buf( key.y, y ) || to_buf( key.x, x )) ? -1 : 0;
    dh_free( &key );
    if (ret) { ios_buf_free( y ); ios_buf_free( x ); }
    return ret;
}

int ios_ltc_dh_public( const unsigned char *p, size_t plen, const unsigned char *g, size_t glen,
                       const unsigned char *x, size_t xlen, struct ios_buf *y )
{
    dh_key key;
    int ret;

    memset( y, 0, sizeof(*y) );
    if (ios_ltc_init()) return -1;
    memset( &key, 0, sizeof(key) );
    if (dh_set_pg( p, plen, g, glen, &key ) != CRYPT_OK) return -1;
    if (dh_set_key( x, xlen, PK_PRIVATE, &key ) != CRYPT_OK) return -1;     /* frees the key on failure */
    ret = to_buf( key.y, y );
    dh_free( &key );
    return ret;
}

int ios_ltc_dh_shared( const unsigned char *p, size_t plen, const unsigned char *g, size_t glen,
                       const unsigned char *x, size_t xlen, const unsigned char *peer_y, size_t ylen,
                       struct ios_buf *secret )
{
    dh_key priv, pub;
    unsigned long len = plen + 1;
    int ret = -1;

    memset( secret, 0, sizeof(*secret) );
    if (ios_ltc_init()) return -1;
    memset( &priv, 0, sizeof(priv) );
    memset( &pub, 0, sizeof(pub) );
    if (dh_set_pg( p, plen, g, glen, &priv ) != CRYPT_OK) return -1;
    if (dh_set_key( x, xlen, PK_PRIVATE, &priv ) != CRYPT_OK) return -1;
    if (dh_set_pg( p, plen, g, glen, &pub ) != CRYPT_OK) { dh_free( &priv ); return -1; }
    if (dh_set_key( peer_y, ylen, PK_PUBLIC, &pub ) != CRYPT_OK) { dh_free( &priv ); return -1; }
    if ((secret->data = malloc( len )) &&
        dh_shared_secret( &priv, &pub, secret->data, &len ) == CRYPT_OK)
    {
        secret->len = len;
        ret = 0;
    }
    else ios_buf_free( secret );
    dh_free( &priv );
    dh_free( &pub );
    return ret;
}

/* ---------------------------------------------------------------- DSA */

int ios_ltc_dsa_generate( unsigned int bits, struct ios_buf *p, struct ios_buf *q, struct ios_buf *g,
                          struct ios_buf *y, struct ios_buf *x )
{
    dsa_key key;
    int ret;

    memset( p, 0, sizeof(*p) ); memset( q, 0, sizeof(*q) ); memset( g, 0, sizeof(*g) );
    memset( y, 0, sizeof(*y) ); memset( x, 0, sizeof(*x) );
    if (ios_ltc_init()) return -1;
    memset( &key, 0, sizeof(key) );
    if (dsa_make_key( NULL, prng_index, 20, (int)(bits / 8), &key ) != CRYPT_OK) return -1;
    ret = (to_buf( key.p, p ) || to_buf( key.q, q ) || to_buf( key.g, g ) ||
           to_buf( key.y, y ) || to_buf( key.x, x )) ? -1 : 0;
    dsa_free( &key );
    if (ret) { ios_buf_free( p ); ios_buf_free( q ); ios_buf_free( g ); ios_buf_free( y ); ios_buf_free( x ); }
    return ret;
}

static int dsa_load( const unsigned char *p, size_t plen, const unsigned char *q, size_t qlen,
                     const unsigned char *g, size_t glen, const unsigned char *k, size_t klen,
                     int type, dsa_key *key )
{
    memset( key, 0, sizeof(*key) );
    if (dsa_set_pqg( p, plen, q, qlen, g, glen, key ) != CRYPT_OK) return -1;
    if (dsa_set_key( k, klen, type, key ) != CRYPT_OK) return -1;   /* frees the key on failure */
    return 0;
}

int ios_ltc_dsa_public( const unsigned char *p, size_t plen, const unsigned char *q, size_t qlen,
                        const unsigned char *g, size_t glen, const unsigned char *x, size_t xlen,
                        struct ios_buf *y )
{
    dsa_key key;
    int ret;

    memset( y, 0, sizeof(*y) );
    if (ios_ltc_init() || dsa_load( p, plen, q, qlen, g, glen, x, xlen, PK_PRIVATE, &key )) return -1;
    ret = to_buf( key.y, y );
    dsa_free( &key );
    return ret;
}

static int put_padded( void *a, unsigned char out[20] )
{
    unsigned long n = mp_unsigned_bin_size( a );

    if (n > 20) return -1;
    memset( out, 0, 20 );
    return mp_to_unsigned_bin( a, out + 20 - n ) == CRYPT_OK ? 0 : -1;
}

int ios_ltc_dsa_sign( const unsigned char *p, size_t plen, const unsigned char *q, size_t qlen,
                      const unsigned char *g, size_t glen, const unsigned char *x, size_t xlen,
                      const unsigned char *hash, size_t hashlen, unsigned char r[20], unsigned char s[20] )
{
    dsa_key key;
    void *mr, *ms;
    int ret = -1;

    if (ios_ltc_init() || dsa_load( p, plen, q, qlen, g, glen, x, xlen, PK_PRIVATE, &key )) return -1;
    if (mp_init_multi( &mr, &ms, NULL ) == CRYPT_OK)
    {
        if (dsa_sign_hash_raw( hash, hashlen, mr, ms, NULL, prng_index, &key ) == CRYPT_OK &&
            !put_padded( mr, r ) && !put_padded( ms, s ))
            ret = 0;
        mp_clear_multi( mr, ms, NULL );
    }
    dsa_free( &key );
    return ret;
}

int ios_ltc_dsa_verify( const unsigned char *p, size_t plen, const unsigned char *q, size_t qlen,
                        const unsigned char *g, size_t glen, const unsigned char *y, size_t ylen,
                        const unsigned char *hash, size_t hashlen, const unsigned char *r, size_t rlen,
                        const unsigned char *s, size_t slen )
{
    dsa_key key;
    void *mr, *ms;
    int stat = 0, ret = -1;

    if (ios_ltc_init() || dsa_load( p, plen, q, qlen, g, glen, y, ylen, PK_PUBLIC, &key )) return -1;
    if (mp_init_multi( &mr, &ms, NULL ) == CRYPT_OK)
    {
        if (!read_mp( mr, r, rlen ) && !read_mp( ms, s, slen ) &&
            dsa_verify_hash_raw( mr, ms, hash, hashlen, &stat, &key ) == CRYPT_OK)
            ret = stat ? 1 : 0;
        else ret = 0;
        mp_clear_multi( mr, ms, NULL );
    }
    dsa_free( &key );
    return ret;
}
