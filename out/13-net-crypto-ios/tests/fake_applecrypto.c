// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 15: a host stand-in for include/ios_applecrypto.h, built on BearSSL's
// AES, DES, RSA and EC code. It behaves like the Apple calls it replaces as far as the callers
// can see (formats, padding, which inputs are refused), counts the calls of every SecKey
// algorithm and route, and counts live objects so a test can check that everything created was
// released (clarification 9). Test code: it never ships.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "ios_der.h"
#include "fake_applecrypto.h"

/* ---------------------------------------------------------------- counters */

static int live;
static unsigned int alg_calls[IOS_AC_ALG_COUNT];
static unsigned int route_calls[FAKE_AC_ROUTES];

int fake_ac_live( void ) { return live; }
unsigned int fake_ac_alg_calls( enum ios_ac_alg alg ) { return alg_calls[alg]; }
unsigned int fake_ac_route_calls( enum fake_ac_route route ) { return route_calls[route]; }
void fake_ac_reset_counts( void )
{
    memset( alg_calls, 0, sizeof(alg_calls) );
    memset( route_calls, 0, sizeof(route_calls) );
}

/* ---------------------------------------------------------------- randomness */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

int ios_ac_random( void *buf, size_t len )
{
    unsigned char *p = buf;
    size_t i;

    route_calls[FAKE_AC_RANDOM]++;
    for (i = 0; i < len; i++)
    {
        rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
        p[i] = (unsigned char)(rng_state >> 24);
    }
    return IOS_AC_OK;
}

static void drbg( br_hmac_drbg_context *ctx )
{
    unsigned char seed[32];
    ios_ac_random( seed, sizeof(seed) );
    br_hmac_drbg_init( ctx, &br_sha256_vtable, seed, sizeof(seed) );
}

/* ---------------------------------------------------------------- CommonCrypto */

struct ios_ac_cryptor
{
    int encrypt;
    enum ios_ac_cipher cipher;
    enum ios_ac_mode mode;
    unsigned char iv[16];
    size_t block;
    union { br_aes_ct64_cbcenc_keys aes_e; br_des_ct_cbcenc_keys des_e; } enc;
    union { br_aes_ct64_cbcdec_keys aes_d; br_des_ct_cbcdec_keys des_d; } dec;
};

int ios_ac_cryptor_create( int encrypt, enum ios_ac_cipher cipher, enum ios_ac_mode mode,
                           const void *key, size_t key_len, const void *iv, ios_ac_cryptor **ret )
{
    struct ios_ac_cryptor *c;

    route_calls[FAKE_AC_CRYPTOR_CREATE]++;
    if (cipher == IOS_AC_AES && key_len != 16 && key_len != 24 && key_len != 32) return IOS_AC_FAIL;
    if (cipher == IOS_AC_3DES && key_len != 24) return IOS_AC_FAIL;
    if (cipher == IOS_AC_3DES && mode != IOS_AC_CBC) return IOS_AC_UNSUPPORTED;
    if (!(c = calloc( 1, sizeof(*c) ))) return IOS_AC_FAIL;
    c->encrypt = encrypt;
    c->cipher = cipher;
    c->mode = mode;
    c->block = cipher == IOS_AC_AES ? 16 : 8;
    if (iv && mode != IOS_AC_ECB) memcpy( c->iv, iv, c->block );
    if (cipher == IOS_AC_AES)
    {
        br_aes_ct64_cbcenc_init( &c->enc.aes_e, key, key_len );
        br_aes_ct64_cbcdec_init( &c->dec.aes_d, key, key_len );
    }
    else
    {
        br_des_ct_cbcenc_init( &c->enc.des_e, key, key_len );
        br_des_ct_cbcdec_init( &c->dec.des_d, key, key_len );
    }
    live++;
    *ret = c;
    return IOS_AC_OK;
}

/* the raw block function: CBC with a zero IV over one block */
static void block_encrypt( struct ios_ac_cryptor *c, unsigned char *b )
{
    unsigned char zero[16] = { 0 };
    if (c->cipher == IOS_AC_AES) br_aes_ct64_cbcenc_run( &c->enc.aes_e, zero, b, 16 );
    else br_des_ct_cbcenc_run( &c->enc.des_e, zero, b, 8 );
}

int ios_ac_cryptor_update( ios_ac_cryptor *c, const void *in, size_t len, void *out )
{
    unsigned char *o = out;
    size_t i;

    if (!len) return IOS_AC_OK;
    if (c->mode != IOS_AC_CFB8 && len % c->block) return IOS_AC_FAIL;
    if (o != in) memmove( o, in, len );
    switch (c->mode)
    {
    case IOS_AC_CBC:
        if (c->cipher == IOS_AC_AES)
        {
            if (c->encrypt) br_aes_ct64_cbcenc_run( &c->enc.aes_e, c->iv, o, len );
            else br_aes_ct64_cbcdec_run( &c->dec.aes_d, c->iv, o, len );
        }
        else
        {
            if (c->encrypt) br_des_ct_cbcenc_run( &c->enc.des_e, c->iv, o, len );
            else br_des_ct_cbcdec_run( &c->dec.des_d, c->iv, o, len );
        }
        break;
    case IOS_AC_ECB:
        for (i = 0; i < len; i += 16)
        {
            unsigned char zero[16] = { 0 };
            if (c->encrypt) br_aes_ct64_cbcenc_run( &c->enc.aes_e, zero, o + i, 16 );
            else br_aes_ct64_cbcdec_run( &c->dec.aes_d, zero, o + i, 16 );
        }
        break;
    case IOS_AC_CFB8:
        for (i = 0; i < len; i++)
        {
            unsigned char ks[16], cbyte;
            memcpy( ks, c->iv, 16 );
            block_encrypt( c, ks );
            cbyte = c->encrypt ? (unsigned char)(o[i] ^ ks[0]) : o[i];
            o[i] ^= ks[0];
            memmove( c->iv, c->iv + 1, 15 );
            c->iv[15] = cbyte;
        }
        break;
    }
    return IOS_AC_OK;
}

void ios_ac_cryptor_release( ios_ac_cryptor *c )
{
    if (!c) return;
    live--;
    free( c );
}

/* ---------------------------------------------------------------- keys */

struct ios_ac_key
{
    int refs;
    enum ios_ac_key_type type;
    int is_private;
    unsigned int bits;
    /* RSA: big-endian parts */
    unsigned char *rsa[8];            /* n e d p q dp dq qinv */
    size_t rsa_len[8];
    /* EC */
    int curve;
    unsigned char q[133];
    size_t qlen;
    unsigned char x[66];
    size_t xlen;
};

enum { RN, RE, RD, RP, RQ, RDP, RDQ, RQI };

static struct ios_ac_key *new_key( enum ios_ac_key_type type, int is_private )
{
    struct ios_ac_key *k = calloc( 1, sizeof(*k) );
    if (!k) return NULL;
    k->refs = 1;
    k->type = type;
    k->is_private = is_private;
    live++;
    return k;
}

static size_t bitlen_of( const unsigned char *p, size_t len )
{
    unsigned char t;
    size_t bits;
    while (len && !*p) { p++; len--; }
    if (!len) return 0;
    bits = len * 8;
    for (t = p[0]; !(t & 0x80); t <<= 1) bits--;
    return bits;
}

static int set_part( struct ios_ac_key *k, int i, const unsigned char *data, size_t len )
{
    struct ios_be b = ios_be_trim( data, len );
    if (!(k->rsa[i] = malloc( b.len ? b.len : 1 ))) return -1;
    if (b.len) memcpy( k->rsa[i], b.data, b.len );
    k->rsa_len[i] = b.len;
    return 0;
}

static int curve_of_size( size_t coord, int *curve, unsigned int *bits )
{
    switch (coord)
    {
    case 32: *curve = BR_EC_secp256r1; *bits = 256; return 0;
    case 48: *curve = BR_EC_secp384r1; *bits = 384; return 0;
    case 66: *curve = BR_EC_secp521r1; *bits = 521; return 0;
    }
    return -1;
}

static size_t coord_of_curve( int curve )
{
    return curve == BR_EC_secp256r1 ? 32 : curve == BR_EC_secp384r1 ? 48 : 66;
}

int ios_ac_key_create( enum ios_ac_key_type type, int is_private, const unsigned char *data,
                       size_t len, ios_ac_key **ret )
{
    struct ios_ac_key *k;

    route_calls[FAKE_AC_KEY_CREATE]++;
    if (!(k = new_key( type, is_private ))) return IOS_AC_FAIL;
    if (type == IOS_AC_KEY_RSA)
    {
        struct ios_rsa_parts parts;
        int priv;

        if (ios_der_rsa_parse( data, len, &parts, &priv ) || priv != is_private) goto fail;
        if (set_part( k, RN, parts.n.data, parts.n.len ) || set_part( k, RE, parts.e.data, parts.e.len )) goto fail;
        if (priv && (set_part( k, RD, parts.d.data, parts.d.len ) || set_part( k, RP, parts.p.data, parts.p.len ) ||
                     set_part( k, RQ, parts.q.data, parts.q.len ) || set_part( k, RDP, parts.dp.data, parts.dp.len ) ||
                     set_part( k, RDQ, parts.dq.data, parts.dq.len ) || set_part( k, RQI, parts.qinv.data, parts.qinv.len )))
            goto fail;
        k->bits = bitlen_of( k->rsa[RN], k->rsa_len[RN] );
    }
    else
    {
        size_t coord;
        if (!len || data[0] != 4) goto fail;
        coord = is_private ? (len - 1) / 3 : (len - 1) / 2;
        if (curve_of_size( coord, &k->curve, &k->bits )) goto fail;
        if (len != 1 + coord * (is_private ? 3 : 2)) goto fail;
        k->qlen = 1 + 2 * coord;
        memcpy( k->q, data, k->qlen );
        if (is_private) { k->xlen = coord; memcpy( k->x, data + k->qlen, coord ); }
    }
    *ret = k;
    return IOS_AC_OK;
fail:
    ios_ac_key_release( k );
    return IOS_AC_FAIL;
}

static unsigned int rsa_min = 1024, rsa_max = 4096;
void fake_ac_set_rsa_generate_range( unsigned int min_bits, unsigned int max_bits )
{
    rsa_min = min_bits;
    rsa_max = max_bits;
}

int ios_ac_key_generate( enum ios_ac_key_type type, unsigned int bits, ios_ac_key **ret )
{
    br_hmac_drbg_context rng;
    struct ios_ac_key *k;

    route_calls[FAKE_AC_KEY_GENERATE]++;
    drbg( &rng );
    if (type == IOS_AC_KEY_RSA)
    {
        unsigned char kpriv[BR_RSA_KBUF_PRIV_SIZE(4096)], kpub[BR_RSA_KBUF_PUB_SIZE(4096)], d[512];
        br_rsa_private_key sk;
        br_rsa_public_key pk;
        size_t dlen;

        if (bits < rsa_min || bits > rsa_max)
        {
            route_calls[FAKE_AC_KEY_GENERATE_REFUSED]++;
            return IOS_AC_UNSUPPORTED;
        }
        if (!br_rsa_keygen_get_default()( &rng.vtable, &sk, kpriv, &pk, kpub, bits, 65537 )) return IOS_AC_FAIL;
        if (!(dlen = br_rsa_compute_privexp_get_default()( d, &sk, 65537 ))) return IOS_AC_FAIL;
        if (!(k = new_key( type, 1 ))) return IOS_AC_FAIL;
        if (set_part( k, RN, pk.n, pk.nlen ) || set_part( k, RE, pk.e, pk.elen ) || set_part( k, RD, d, dlen ) ||
            set_part( k, RP, sk.p, sk.plen ) || set_part( k, RQ, sk.q, sk.qlen ) || set_part( k, RDP, sk.dp, sk.dplen ) ||
            set_part( k, RDQ, sk.dq, sk.dqlen ) || set_part( k, RQI, sk.iq, sk.iqlen ))
        {
            ios_ac_key_release( k );
            return IOS_AC_FAIL;
        }
        k->bits = bits;
    }
    else
    {
        unsigned char kbuf[BR_EC_KBUF_PRIV_MAX_SIZE], pbuf[BR_EC_KBUF_PUB_MAX_SIZE];
        br_ec_private_key sk;
        br_ec_public_key pk;
        int curve;
        unsigned int b;

        if (bits == 256) curve = BR_EC_secp256r1;
        else if (bits == 384) curve = BR_EC_secp384r1;
        else if (bits == 521) curve = BR_EC_secp521r1;
        else { route_calls[FAKE_AC_KEY_GENERATE_REFUSED]++; return IOS_AC_UNSUPPORTED; }
        if (!br_ec_keygen( &rng.vtable, br_ec_get_default(), &sk, kbuf, curve )) return IOS_AC_FAIL;
        if (!br_ec_compute_pub( br_ec_get_default(), &pk, pbuf, &sk )) return IOS_AC_FAIL;
        if (!(k = new_key( type, 1 ))) return IOS_AC_FAIL;
        curve_of_size( coord_of_curve( curve ), &k->curve, &b );
        k->bits = b;
        k->qlen = pk.qlen;
        memcpy( k->q, pk.q, pk.qlen );
        /* the private scalar, left-padded to the coordinate size as Apple's X9.63 has it */
        k->xlen = coord_of_curve( curve );
        memset( k->x, 0, k->xlen );
        memcpy( k->x + k->xlen - sk.xlen, sk.x, sk.xlen );
    }
    *ret = k;
    return IOS_AC_OK;
}

int ios_ac_key_copy_public( ios_ac_key *key, ios_ac_key **ret )
{
    struct ios_ac_key *k = new_key( key->type, 0 );
    int i;

    if (!k) return IOS_AC_FAIL;
    k->bits = key->bits;
    if (key->type == IOS_AC_KEY_RSA)
    {
        for (i = RN; i <= RE; i++)
            if (set_part( k, i, key->rsa[i], key->rsa_len[i] )) { ios_ac_key_release( k ); return IOS_AC_FAIL; }
    }
    else
    {
        k->curve = key->curve;
        k->qlen = key->qlen;
        memcpy( k->q, key->q, key->qlen );
    }
    *ret = k;
    return IOS_AC_OK;
}

int ios_ac_key_export( ios_ac_key *key, unsigned char **data, size_t *len )
{
    if (key->type == IOS_AC_KEY_RSA)
    {
        struct ios_rsa_parts p;
        memset( &p, 0, sizeof(p) );
        p.n.data = key->rsa[RN]; p.n.len = key->rsa_len[RN];
        p.e.data = key->rsa[RE]; p.e.len = key->rsa_len[RE];
        if (!key->is_private) return ios_der_rsa_public( &p, data, len ) ? IOS_AC_FAIL : IOS_AC_OK;
        p.d.data = key->rsa[RD]; p.d.len = key->rsa_len[RD];
        p.p.data = key->rsa[RP]; p.p.len = key->rsa_len[RP];
        p.q.data = key->rsa[RQ]; p.q.len = key->rsa_len[RQ];
        p.dp.data = key->rsa[RDP]; p.dp.len = key->rsa_len[RDP];
        p.dq.data = key->rsa[RDQ]; p.dq.len = key->rsa_len[RDQ];
        p.qinv.data = key->rsa[RQI]; p.qinv.len = key->rsa_len[RQI];
        return ios_der_rsa_private( &p, data, len ) ? IOS_AC_FAIL : IOS_AC_OK;
    }
    *len = key->qlen + key->xlen;
    if (!(*data = malloc( *len ))) return IOS_AC_FAIL;
    memcpy( *data, key->q, key->qlen );
    memcpy( *data + key->qlen, key->x, key->xlen );
    return IOS_AC_OK;
}

int ios_ac_key_info( ios_ac_key *key, enum ios_ac_key_type *type, unsigned int *bits )
{
    *type = key->type;
    *bits = key->bits;
    return IOS_AC_OK;
}

ios_ac_key *ios_ac_key_retain( ios_ac_key *key )
{
    if (key) key->refs++;
    return key;
}

void ios_ac_key_release( ios_ac_key *key )
{
    int i;
    if (!key || --key->refs) return;
    for (i = 0; i < 8; i++) free( key->rsa[i] );
    free( key );
    live--;
}

/* ---------------------------------------------------------------- SecKey operations */

static void rsa_pub( const struct ios_ac_key *k, br_rsa_public_key *pk )
{
    pk->n = k->rsa[RN]; pk->nlen = k->rsa_len[RN];
    pk->e = k->rsa[RE]; pk->elen = k->rsa_len[RE];
}

static int rsa_priv( const struct ios_ac_key *k, br_rsa_private_key *sk )
{
    if (!k->is_private) return -1;
    sk->n_bitlen = k->bits;
    sk->p = k->rsa[RP]; sk->plen = k->rsa_len[RP];
    sk->q = k->rsa[RQ]; sk->qlen = k->rsa_len[RQ];
    sk->dp = k->rsa[RDP]; sk->dplen = k->rsa_len[RDP];
    sk->dq = k->rsa[RDQ]; sk->dqlen = k->rsa_len[RDQ];
    sk->iq = k->rsa[RQI]; sk->iqlen = k->rsa_len[RQI];
    return 0;
}

static size_t modlen( const struct ios_ac_key *k ) { return (k->bits + 7) / 8; }

static const br_hash_class *oaep_hash( enum ios_ac_alg alg )
{
    switch (alg)
    {
    case IOS_AC_RSA_ENC_OAEP_SHA1:   return &br_sha1_vtable;
    case IOS_AC_RSA_ENC_OAEP_SHA256: return &br_sha256_vtable;
    case IOS_AC_RSA_ENC_OAEP_SHA384: return &br_sha384_vtable;
    case IOS_AC_RSA_ENC_OAEP_SHA512: return &br_sha512_vtable;
    default: return NULL;
    }
}

int ios_ac_encrypt( ios_ac_key *key, enum ios_ac_alg alg, const void *in, size_t len,
                    unsigned char **out, size_t *out_len )
{
    br_rsa_public_key pk;
    size_t k = modlen( key );
    unsigned char *buf;

    alg_calls[alg]++;
    if (key->type != IOS_AC_KEY_RSA) return IOS_AC_FAIL;
    rsa_pub( key, &pk );
    if (!(buf = malloc( k ))) return IOS_AC_FAIL;
    if (alg == IOS_AC_RSA_ENC_PKCS1)
    {
        size_t i, ps;
        if (len + 11 > k) goto fail;
        ps = k - 3 - len;
        buf[0] = 0; buf[1] = 2;
        ios_ac_random( buf + 2, ps );
        for (i = 0; i < ps; i++) if (!buf[2 + i]) buf[2 + i] = 0x5a;
        buf[2 + ps] = 0;
        memcpy( buf + 3 + ps, in, len );
        if (!br_rsa_public_get_default()( buf, k, &pk )) goto fail;
    }
    else if (alg == IOS_AC_RSA_ENC_RAW)
    {
        if (len > k) goto fail;
        memset( buf, 0, k - len );
        memcpy( buf + k - len, in, len );
        if (!br_rsa_public_get_default()( buf, k, &pk )) goto fail;
    }
    else if (oaep_hash( alg ))
    {
        br_hmac_drbg_context rng;
        drbg( &rng );
        if (br_rsa_oaep_encrypt_get_default()( &rng.vtable, oaep_hash( alg ), NULL, 0, &pk, buf, k, in, len ) != k)
            goto fail;
    }
    else goto fail;
    *out = buf;
    *out_len = k;
    return IOS_AC_OK;
fail:
    free( buf );
    return IOS_AC_FAIL;
}

int ios_ac_decrypt( ios_ac_key *key, enum ios_ac_alg alg, const void *in, size_t len,
                    unsigned char **out, size_t *out_len )
{
    br_rsa_private_key sk;
    size_t k = modlen( key ), n;
    unsigned char *buf;

    alg_calls[alg]++;
    if (key->type != IOS_AC_KEY_RSA || rsa_priv( key, &sk ) || len != k) return IOS_AC_FAIL;
    if (!(buf = malloc( k ))) return IOS_AC_FAIL;
    memcpy( buf, in, k );
    if (oaep_hash( alg ))
    {
        n = k;
        if (!br_rsa_oaep_decrypt_get_default()( oaep_hash( alg ), NULL, 0, &sk, buf, &n )) goto fail;
        *out_len = n;
    }
    else
    {
        if (!br_rsa_private_get_default()( buf, &sk )) goto fail;
        if (alg == IOS_AC_RSA_ENC_PKCS1)
        {
            size_t i = 2;
            if (buf[0] || buf[1] != 2) goto fail;
            while (i < k && buf[i]) i++;
            if (i == k || i < 10) goto fail;
            n = k - i - 1;
            memmove( buf, buf + i + 1, n );
            *out_len = n;
        }
        else if (alg == IOS_AC_RSA_ENC_RAW) *out_len = k;
        else goto fail;
    }
    *out = buf;
    return IOS_AC_OK;
fail:
    free( buf );
    return IOS_AC_FAIL;
}

static const unsigned char *pkcs1_oid( enum ios_ac_alg alg, size_t *hash_len, int *ok )
{
    *ok = 1;
    switch (alg)
    {
    case IOS_AC_RSA_SIG_PKCS1_RAW:    *hash_len = 0;  return NULL;
    case IOS_AC_RSA_SIG_PKCS1_SHA1:   *hash_len = 20; return BR_HASH_OID_SHA1;
    case IOS_AC_RSA_SIG_PKCS1_SHA256: *hash_len = 32; return BR_HASH_OID_SHA256;
    case IOS_AC_RSA_SIG_PKCS1_SHA384: *hash_len = 48; return BR_HASH_OID_SHA384;
    case IOS_AC_RSA_SIG_PKCS1_SHA512: *hash_len = 64; return BR_HASH_OID_SHA512;
    default: *ok = 0; return NULL;
    }
}

static const br_hash_class *pss_hash( enum ios_ac_alg alg )
{
    switch (alg)
    {
    case IOS_AC_RSA_SIG_PSS_SHA1:   return &br_sha1_vtable;
    case IOS_AC_RSA_SIG_PSS_SHA256: return &br_sha256_vtable;
    case IOS_AC_RSA_SIG_PSS_SHA384: return &br_sha384_vtable;
    case IOS_AC_RSA_SIG_PSS_SHA512: return &br_sha512_vtable;
    default: return NULL;
    }
}

static const br_hash_class *hash_for_len( size_t len )
{
    if (len <= 20) return &br_sha1_vtable;
    if (len <= 32) return &br_sha256_vtable;
    if (len <= 48) return &br_sha384_vtable;
    return &br_sha512_vtable;
}

static void ec_pub( const struct ios_ac_key *k, br_ec_public_key *pk )
{
    pk->curve = k->curve;
    pk->q = (unsigned char *)k->q;
    pk->qlen = k->qlen;
}

int ios_ac_sign( ios_ac_key *key, enum ios_ac_alg alg, const void *digest, size_t len,
                 unsigned char **sig, size_t *sig_len )
{
    unsigned char *buf;
    size_t hlen;
    int ok;

    alg_calls[alg]++;
    if (!key->is_private) return IOS_AC_FAIL;
    if (alg == IOS_AC_ECDSA_DIGEST_X962)
    {
        br_ec_private_key sk;
        unsigned char hash[64];
        size_t n;

        if (key->type != IOS_AC_KEY_EC || len > 64) return IOS_AC_FAIL;
        /* BearSSL takes the digest length from the hash class: a zero-padded copy for a short one */
        memset( hash, 0, sizeof(hash) );
        memcpy( hash, digest, len );
        sk.curve = key->curve; sk.x = key->x; sk.xlen = key->xlen;
        if (!(buf = malloc( 160 ))) return IOS_AC_FAIL;
        if (len != 20 && len != 32 && len != 48 && len != 64) { free( buf ); return IOS_AC_FAIL; }
        if (!(n = br_ecdsa_i31_sign_asn1( br_ec_get_default(), hash_for_len( len ), hash, &sk, buf )))
        {
            free( buf );
            return IOS_AC_FAIL;
        }
        *sig = buf;
        *sig_len = n;
        return IOS_AC_OK;
    }
    if (key->type != IOS_AC_KEY_RSA) return IOS_AC_FAIL;
    {
        br_rsa_private_key sk;
        const unsigned char *oid = pkcs1_oid( alg, &hlen, &ok );

        rsa_priv( key, &sk );
        if (!(buf = malloc( modlen( key ) ))) return IOS_AC_FAIL;
        if (ok)
        {
            if (hlen && len != hlen) { free( buf ); return IOS_AC_FAIL; }
            if (!br_rsa_pkcs1_sign_get_default()( oid, digest, len, &sk, buf )) { free( buf ); return IOS_AC_FAIL; }
        }
        else if (pss_hash( alg ))
        {
            const br_hash_class *h = pss_hash( alg );
            br_hmac_drbg_context rng;
            size_t hl = (h->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;

            if (len != hl) { free( buf ); return IOS_AC_FAIL; }
            drbg( &rng );
            if (!br_rsa_pss_sign_get_default()( &rng.vtable, h, h, digest, hl, &sk, buf )) { free( buf ); return IOS_AC_FAIL; }
        }
        else { free( buf ); return IOS_AC_FAIL; }
        *sig = buf;
        *sig_len = modlen( key );
        return IOS_AC_OK;
    }
}

int ios_ac_verify( ios_ac_key *key, enum ios_ac_alg alg, const void *digest, size_t len,
                   const void *sig, size_t sig_len )
{
    size_t hlen;
    int ok;

    alg_calls[alg]++;
    if (alg == IOS_AC_ECDSA_DIGEST_X962)
    {
        br_ec_public_key pk;
        if (key->type != IOS_AC_KEY_EC) return IOS_AC_FAIL;
        ec_pub( key, &pk );
        return br_ecdsa_i31_vrfy_asn1( br_ec_get_default(), digest, len, &pk, sig, sig_len ) ? IOS_AC_OK : IOS_AC_FAIL;
    }
    if (key->type != IOS_AC_KEY_RSA) return IOS_AC_FAIL;
    {
        br_rsa_public_key pk;
        const unsigned char *oid = pkcs1_oid( alg, &hlen, &ok );

        rsa_pub( key, &pk );
        if (ok)
        {
            unsigned char out[512];
            size_t want = hlen ? hlen : len;
            if (hlen && len != hlen) return IOS_AC_FAIL;
            if (want > sizeof(out)) return IOS_AC_FAIL;
            if (!br_rsa_pkcs1_vrfy_get_default()( sig, sig_len, oid, want, &pk, out )) return IOS_AC_FAIL;
            return memcmp( out, digest, want ) ? IOS_AC_FAIL : IOS_AC_OK;
        }
        if (pss_hash( alg ))
        {
            const br_hash_class *h = pss_hash( alg );
            size_t hl = (h->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
            if (len != hl) return IOS_AC_FAIL;
            return br_rsa_pss_vrfy_get_default()( sig, sig_len, h, h, digest, hl, &pk ) ? IOS_AC_OK : IOS_AC_FAIL;
        }
        return IOS_AC_FAIL;
    }
}

int ios_ac_ecdh( ios_ac_key *priv, ios_ac_key *pub, unsigned char **out, size_t *out_len )
{
    const br_ec_impl *impl = br_ec_get_default();
    unsigned char g[133];
    size_t xlen, xoff;

    route_calls[FAKE_AC_ECDH]++;
    if (priv->type != IOS_AC_KEY_EC || pub->type != IOS_AC_KEY_EC || !priv->is_private ||
        priv->curve != pub->curve) return IOS_AC_FAIL;
    memcpy( g, pub->q, pub->qlen );
    if (!impl->mul( g, pub->qlen, priv->x, priv->xlen, priv->curve )) return IOS_AC_FAIL;
    xoff = impl->xoff( priv->curve, &xlen );
    if (!(*out = malloc( xlen ))) return IOS_AC_FAIL;
    memcpy( *out, g + xoff, xlen );
    *out_len = xlen;
    return IOS_AC_OK;
}

/* ---------------------------------------------------------------- certificates */

static unsigned char *distrust;
static size_t distrust_len;

void fake_ac_set_distrust_marker( const void *marker, size_t len )
{
    free( distrust );
    distrust = NULL;
    distrust_len = 0;
    if (marker && len && (distrust = malloc( len ))) { memcpy( distrust, marker, len ); distrust_len = len; }
}

int ios_ac_cert_is_trusted_anchor( const unsigned char *der, size_t len )
{
    size_t i;

    route_calls[FAKE_AC_TRUST]++;
    if (len < 4 || der[0] != 0x30) return IOS_AC_FAIL;
    if (distrust)
        for (i = 0; i + distrust_len <= len; i++)
            if (!memcmp( der + i, distrust, distrust_len )) return 0;
    return 1;
}

/* ---------------------------------------------------------------- PKCS #12 */

static struct
{
    int32_t status;
    uint16_t password[64];
    int any_password;
    unsigned char *certs[16];
    size_t cert_lens[16];
    unsigned int cert_count, identities;
    unsigned char *key_der;
    size_t key_der_len;
} p12cfg = { -25300 /* errSecItemNotFound */, { 0 }, 1, { 0 }, { 0 }, 0, 0, NULL, 0 };

void fake_ac_set_pkcs12( int32_t status, const uint16_t *password,
                         const unsigned char *const *certs, const size_t *cert_lens,
                         unsigned int cert_count, unsigned int identities,
                         const unsigned char *key_der, size_t key_der_len )
{
    unsigned int i;

    for (i = 0; i < p12cfg.cert_count; i++) free( p12cfg.certs[i] );
    free( p12cfg.key_der );
    memset( &p12cfg, 0, sizeof(p12cfg) );
    p12cfg.status = status;
    p12cfg.any_password = !password;
    for (i = 0; password && password[i] && i < 63; i++) p12cfg.password[i] = password[i];
    for (i = 0; i < cert_count && i < 16; i++)
    {
        p12cfg.certs[i] = malloc( cert_lens[i] );
        memcpy( p12cfg.certs[i], certs[i], cert_lens[i] );
        p12cfg.cert_lens[i] = cert_lens[i];
    }
    p12cfg.cert_count = i;
    p12cfg.identities = identities;
    if (key_der)
    {
        p12cfg.key_der = malloc( key_der_len );
        memcpy( p12cfg.key_der, key_der, key_der_len );
        p12cfg.key_der_len = key_der_len;
    }
}

int ios_ac_pkcs12_import( const void *pfx, size_t len, const uint16_t *password,
                          struct ios_ac_p12 **ret, int32_t *status )
{
    struct ios_ac_p12 *p12;
    unsigned int i, j;

    route_calls[FAKE_AC_PKCS12]++;
    *status = 0;
    if (p12cfg.status) { *status = p12cfg.status; return IOS_AC_FAIL; }
    if (!p12cfg.any_password)
    {
        static const uint16_t empty[1] = { 0 };
        const uint16_t *pw = password ? password : empty;
        for (i = 0; pw[i] && pw[i] == p12cfg.password[i]; i++) ;
        if (pw[i] != p12cfg.password[i]) { *status = -25293; /* errSecAuthFailed */ return IOS_AC_FAIL; }
    }
    if (!(p12 = calloc( 1, sizeof(*p12) ))) return IOS_AC_FAIL;
    live++;
    p12->certs = calloc( p12cfg.cert_count + 1, sizeof(*p12->certs) );
    p12->cert_lens = calloc( p12cfg.cert_count + 1, sizeof(*p12->cert_lens) );
    for (i = 0; i < p12cfg.cert_count; i++)
    {
        for (j = 0; j < p12->cert_count; j++)
            if (p12->cert_lens[j] == p12cfg.cert_lens[i] && !memcmp( p12->certs[j], p12cfg.certs[i], p12cfg.cert_lens[i] ))
                break;
        if (j < p12->cert_count) continue;
        p12->certs[p12->cert_count] = malloc( p12cfg.cert_lens[i] );
        memcpy( p12->certs[p12->cert_count], p12cfg.certs[i], p12cfg.cert_lens[i] );
        p12->cert_lens[p12->cert_count++] = p12cfg.cert_lens[i];
    }
    p12->identities = p12cfg.identities;
    if (p12cfg.key_der && ios_ac_key_create( IOS_AC_KEY_RSA, 1, p12cfg.key_der, p12cfg.key_der_len, &p12->key ))
    {
        ios_ac_pkcs12_free( p12 );
        return IOS_AC_FAIL;
    }
    *ret = p12;
    (void)pfx; (void)len;
    return IOS_AC_OK;
}

void ios_ac_pkcs12_free( struct ios_ac_p12 *p12 )
{
    unsigned int i;

    if (!p12) return;
    for (i = 0; i < p12->cert_count; i++) free( p12->certs[i] );
    free( p12->certs );
    free( p12->cert_lens );
    ios_ac_key_release( p12->key );
    free( p12 );
    live--;
}
