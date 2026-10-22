/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
 * Copyright 2018 Hans Leidekker for CodeWeavers
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
 * Changed for MYIOSDECK (2026, spec 13 clarifications 5-9 and 13, and Part B): starts from
 * Wine 11.4's dlls/bcrypt/gnutls.c; GnuTLS replaced by Security.framework, CommonCrypto, BearSSL
 * and LibTomCrypt. The unix-call table, its order, every argument check, status code, size rule
 * and BCRYPT / CAPI blob layout are upstream's; the WoW64 thunks convert guest addresses with
 * ios_g2h(). See out/13-net-crypto-ios/CHANGES.md for the backend of each operation and the
 * behaviour differences.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sched.h>
#include <sys/types.h>

#include "bearssl/bearssl.h"

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntsecapi.h"
#include "wincrypt.h"
#include "bcrypt.h"

#include "bcrypt_internal.h"

#include "wine/debug.h"
#include "ios_guest.h"
#include "ios_applecrypto.h"
#include "ios_der.h"
#include "ios_ltc.h"

WINE_DEFAULT_DEBUG_CHANNEL(bcrypt);

/* ---------------------------------------------------------------- key state (clarification 8.2) */

/* a big-endian byte string; NULL / 0 when absent */
struct bytes
{
    UCHAR *data;
    ULONG  len;
};

/* GCM through BearSSL */
struct gcm
{
    br_aes_ct64_ctr_keys aes;
    br_gcm_context       ctx;
    BOOL                 flipped;   /* the first encrypt or decrypt has run */
};

/* a CommonCrypto cryptor, with the chain state kept so a change of direction continues it, as one
 * GnuTLS handle did for both directions */
struct sym
{
    ios_ac_cryptor *cryptor;
    BOOL            encrypt;
    UCHAR           iv[16];
};

/* what key->private[0] points to */
struct key_impl
{
    /* symmetric */
    struct sym *sym;
    struct gcm *gcm;
    /* asymmetric: the canonical material (clarification 8.3) */
    BOOL priv, pub, params;
    struct bytes n, e, d, p, q, dp, dq, qinv;      /* RSA */
    struct bytes ec_x, ec_y, ec_d;                 /* EC: coordinates of the curve's size; X25519: x and d */
    struct bytes dh_p, dh_g, dh_y, dh_x;           /* DH */
    struct bytes dsa_p, dsa_q, dsa_g, dsa_y, dsa_x;/* DSA */
    /* made from the material on first use */
    ios_ac_key *sec_priv, *sec_pub;
};

C_ASSERT( sizeof(struct key_impl *) <= sizeof(((struct key *)0)->private) );

static struct key_impl *key_impl( struct key *key )
{
    return (struct key_impl *)(ULONG_PTR)key->private[0];
}

static struct key_impl *key_impl_get( struct key *key )
{
    struct key_impl *impl = key_impl( key );

    if (!impl && (impl = calloc( 1, sizeof(*impl) ))) key->private[0] = (ULONG_PTR)impl;
    return impl;
}

static void bytes_free( struct bytes *b )
{
    free( b->data );
    b->data = NULL;
    b->len = 0;
}

/* set b to a copy of data (leading zeros stripped when trim) */
static BOOL bytes_set( struct bytes *b, const UCHAR *data, ULONG len, BOOL trim )
{
    UCHAR *copy;

    if (trim) while (len && !*data) { data++; len--; }
    if (!(copy = malloc( len ? len : 1 ))) return FALSE;
    if (len) memcpy( copy, data, len );
    free( b->data );
    b->data = copy;
    b->len = len;
    return TRUE;
}

/* a copy of data left-padded (or with leading zeros cut) to exactly size bytes */
static BOOL bytes_set_fixed( struct bytes *b, const UCHAR *data, ULONG len, ULONG size )
{
    UCHAR *copy;

    while (len > size && !*data) { data++; len--; }
    if (len > size || !(copy = calloc( 1, size ? size : 1 ))) return FALSE;
    memcpy( copy + size - len, data, len );
    free( b->data );
    b->data = copy;
    b->len = size;
    return TRUE;
}

static BOOL bytes_take( struct bytes *b, struct ios_buf *buf )
{
    free( b->data );
    b->data = buf->data;
    b->len = buf->len;
    buf->data = NULL;
    buf->len = 0;
    return TRUE;
}

static void release_sec( struct key_impl *impl )
{
    ios_ac_key_release( impl->sec_priv );
    ios_ac_key_release( impl->sec_pub );
    impl->sec_priv = impl->sec_pub = NULL;
}

static void clear_asymmetric( struct key_impl *impl )
{
    struct bytes *all[] = { &impl->n, &impl->e, &impl->d, &impl->p, &impl->q, &impl->dp, &impl->dq, &impl->qinv,
                            &impl->ec_x, &impl->ec_y, &impl->ec_d, &impl->dh_p, &impl->dh_g, &impl->dh_y,
                            &impl->dh_x, &impl->dsa_p, &impl->dsa_q, &impl->dsa_g, &impl->dsa_y, &impl->dsa_x };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(all); i++) bytes_free( all[i] );
    release_sec( impl );
    impl->priv = impl->pub = impl->params = FALSE;
}

static void clear_private( struct key_impl *impl )
{
    bytes_free( &impl->d ); bytes_free( &impl->p ); bytes_free( &impl->q ); bytes_free( &impl->dp );
    bytes_free( &impl->dq ); bytes_free( &impl->qinv ); bytes_free( &impl->ec_d ); bytes_free( &impl->dh_x );
    bytes_free( &impl->dsa_x );
    ios_ac_key_release( impl->sec_priv );
    impl->sec_priv = NULL;
    impl->priv = FALSE;
}

/* ---------------------------------------------------------------- the ASN.1 writer of upstream */

struct buffer
{
    BYTE  *buffer;
    DWORD  length;
    DWORD  pos;
    BOOL   error;
};

static void buffer_init( struct buffer *buffer )
{
    buffer->buffer = NULL;
    buffer->length = 0;
    buffer->pos    = 0;
    buffer->error  = FALSE;
}

static void buffer_free( struct buffer *buffer )
{
    free( buffer->buffer );
}

static void buffer_append( struct buffer *buffer, BYTE *data, DWORD len )
{
    if (!len) return;

    if (buffer->pos + len > buffer->length)
    {
        DWORD new_length = max( max( buffer->pos + len, buffer->length * 2 ), 64 );
        BYTE *new_buffer;

        if (!(new_buffer = realloc( buffer->buffer, new_length )))
        {
            ERR( "out of memory\n" );
            buffer->error = TRUE;
            return;
        }

        buffer->buffer = new_buffer;
        buffer->length = new_length;
    }

    memcpy( &buffer->buffer[buffer->pos], data, len );
    buffer->pos += len;
}

static void buffer_append_byte( struct buffer *buffer, BYTE value )
{
    buffer_append( buffer, &value, sizeof(value) );
}

static void buffer_append_asn1_length( struct buffer *buffer, DWORD length )
{
    DWORD num_bytes;

    if (length < 128)
    {
        buffer_append_byte( buffer, length );
        return;
    }

    if (length <= 0xff) num_bytes = 1;
    else if (length <= 0xffff) num_bytes = 2;
    else if (length <= 0xffffff) num_bytes = 3;
    else num_bytes = 4;

    buffer_append_byte( buffer, 0x80 | num_bytes );
    while (num_bytes--) buffer_append_byte( buffer, length >> (num_bytes * 8) );
}

static void buffer_append_asn1_integer( struct buffer *buffer, BYTE *data, DWORD len )
{
    DWORD leading_zero = (*data & 0x80) != 0;

    buffer_append_byte( buffer, 0x02 );  /* tag */
    buffer_append_asn1_length( buffer, len + leading_zero );
    if (leading_zero) buffer_append_byte( buffer, 0 );
    buffer_append( buffer, data, len );
}

static void buffer_append_asn1_sequence( struct buffer *buffer, struct buffer *content )
{
    if (content->error)
    {
        buffer->error = TRUE;
        return;
    }

    buffer_append_byte( buffer, 0x30 );  /* tag */
    buffer_append_asn1_length( buffer, content->pos );
    buffer_append( buffer, content->buffer, content->pos );
}

static void buffer_append_asn1_r_s( struct buffer *buffer, BYTE *r, DWORD r_len, BYTE *s, DWORD s_len )
{
    struct buffer value;

    buffer_init( &value );
    buffer_append_asn1_integer( &value, r, r_len );
    buffer_append_asn1_integer( &value, s, s_len );
    buffer_append_asn1_sequence( buffer, &value );
    buffer_free( &value );
}

/* ---------------------------------------------------------------- symmetric keys */

static NTSTATUS key_symmetric_vector_reset( void *args )
{
    struct key *key = args;
    struct key_impl *impl = key_impl( key );

    if (!impl) return STATUS_SUCCESS;
    if (impl->sym)
    {
        TRACE( "invalidating cipher handle\n" );
        ios_ac_cryptor_release( impl->sym->cryptor );
        free( impl->sym );
        impl->sym = NULL;
    }
    if (impl->gcm)
    {
        TRACE( "invalidating cipher handle\n" );
        free( impl->gcm );
        impl->gcm = NULL;
    }
    return STATUS_SUCCESS;
}

/* which backend a symmetric key uses; upstream's get_gnutls_cipher mapping */
enum sym_kind { SYM_NONE, SYM_CC, SYM_GCM };

static enum sym_kind get_sym_kind( const struct key *key, enum ios_ac_cipher *cipher, enum ios_ac_mode *mode )
{
    switch (key->alg_id)
    {
    case ALG_ID_3DES:
        WARN( "handle block size\n" );
        if (key->u.s.mode == CHAIN_MODE_CBC)
        {
            *cipher = IOS_AC_3DES;
            *mode = IOS_AC_CBC;
            return SYM_CC;
        }
        FIXME( "3DES mode %u with key length %u not supported\n", key->u.s.mode, key->u.s.secret_len );
        return SYM_NONE;

    case ALG_ID_AES:
        WARN( "handle block size\n" );
        *cipher = IOS_AC_AES;
        switch (key->u.s.mode)
        {
        case CHAIN_MODE_GCM:
            if (key->u.s.secret_len == 16 || key->u.s.secret_len == 32) return SYM_GCM;
            break;
        case CHAIN_MODE_ECB:
            if (key->u.s.secret_len == 16 || key->u.s.secret_len == 24 || key->u.s.secret_len == 32)
            {
                *mode = IOS_AC_ECB;
                return SYM_CC;
            }
            break;
        case CHAIN_MODE_CBC:
            if (key->u.s.secret_len == 16 || key->u.s.secret_len == 24 || key->u.s.secret_len == 32)
            {
                *mode = IOS_AC_CBC;
                return SYM_CC;
            }
            break;
        case CHAIN_MODE_CFB:
            if (key->u.s.secret_len == 16 || key->u.s.secret_len == 24 || key->u.s.secret_len == 32)
            {
                *mode = IOS_AC_CFB8;
                return SYM_CC;
            }
            break;
        default:
            break;
        }
        FIXME( "AES mode %u with key length %u not supported\n", key->u.s.mode, key->u.s.secret_len );
        return SYM_NONE;

    default:
        FIXME( "algorithm %u not supported\n", key->alg_id );
        return SYM_NONE;
    }
}

/* the IV of a new chain: key->u.s.vector, zero-padded to the block, or zeros (as GnuTLS) */
static void initial_iv( const struct key *key, UCHAR *iv, ULONG block )
{
    memset( iv, 0, 16 );
    if (key->u.s.vector) memcpy( iv, key->u.s.vector, min( key->u.s.vector_len, block ) );
}

static NTSTATUS init_cipher_handle( struct key *key )
{
    struct key_impl *impl;
    enum ios_ac_cipher cipher = IOS_AC_AES;
    enum ios_ac_mode mode = IOS_AC_CBC;
    enum sym_kind kind;

    if ((impl = key_impl( key )) && (impl->sym || impl->gcm)) return STATUS_SUCCESS;
    if ((kind = get_sym_kind( key, &cipher, &mode )) == SYM_NONE) return STATUS_NOT_SUPPORTED;
    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    if (kind == SYM_GCM)
    {
        struct gcm *gcm;

        if (!(gcm = calloc( 1, sizeof(*gcm) ))) return STATUS_NO_MEMORY;
        br_aes_ct64_ctr_init( &gcm->aes, key->u.s.secret, key->u.s.secret_len );
        br_gcm_init( &gcm->ctx, &gcm->aes.vtable, br_ghash_ctmul64 );
        br_gcm_reset( &gcm->ctx, key->u.s.vector, key->u.s.vector ? key->u.s.vector_len : 0 );
        impl->gcm = gcm;
        return STATUS_SUCCESS;
    }

    if (!(impl->sym = calloc( 1, sizeof(*impl->sym) ))) return STATUS_NO_MEMORY;
    initial_iv( key, impl->sym->iv, cipher == IOS_AC_AES ? 16 : 8 );
    /* the cryptor itself is made by the first encrypt or decrypt, which knows the direction */
    return STATUS_SUCCESS;
}

/* advance iv over the ciphertext ct of len bytes */
static void chain_state( enum ios_ac_mode mode, ULONG block, UCHAR *iv, const UCHAR *ct, ULONG len )
{
    UCHAR reg[32];

    if (mode == IOS_AC_CBC && len >= block) memcpy( iv, ct + len - block, block );
    else if (mode == IOS_AC_CFB8 && len >= 16) memcpy( iv, ct + len - 16, 16 );
    else if (mode == IOS_AC_CFB8 && len)
    {
        memcpy( reg, iv, 16 );
        memcpy( reg + 16, ct, len );
        memcpy( iv, reg + len, 16 );
    }
}

/* run CommonCrypto in the call's direction, continuing the chain state */
static NTSTATUS sym_run( struct key *key, BOOL encrypt, const UCHAR *input, ULONG len, UCHAR *output )
{
    struct sym *sym = key_impl( key )->sym;
    enum ios_ac_cipher cipher = IOS_AC_AES;
    enum ios_ac_mode mode = IOS_AC_CBC;
    ULONG block;
    UCHAR next_iv[16];

    get_sym_kind( key, &cipher, &mode );
    block = cipher == IOS_AC_AES ? 16 : 8;

    if (sym->cryptor && sym->encrypt != encrypt)
    {
        ios_ac_cryptor_release( sym->cryptor );
        sym->cryptor = NULL;
    }
    if (!sym->cryptor)
    {
        if (ios_ac_cryptor_create( encrypt, cipher, mode, key->u.s.secret, key->u.s.secret_len,
                                   mode == IOS_AC_ECB ? NULL : sym->iv, &sym->cryptor ))
            return STATUS_INTERNAL_ERROR;
        sym->encrypt = encrypt;
    }

    /* the chain state after this call, kept for a later change of direction: the last ciphertext
     * block (CBC) or the last 16 ciphertext bytes (CFB8). Decrypting, the ciphertext is the input,
     * which may be the output too, so it is saved first. */
    memcpy( next_iv, sym->iv, sizeof(next_iv) );
    if (!encrypt) chain_state( mode, block, next_iv, input, len );

    if (ios_ac_cryptor_update( sym->cryptor, input, len, output )) return STATUS_INTERNAL_ERROR;

    if (encrypt) chain_state( mode, block, next_iv, output, len );
    memcpy( sym->iv, next_iv, sizeof(next_iv) );
    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_set_auth_data( void *args )
{
    const struct key_symmetric_set_auth_data_params *params = args;
    struct key_impl *impl;
    NTSTATUS status;

    if (!params->auth_data) return STATUS_SUCCESS;
    if ((status = init_cipher_handle( params->key ))) return status;

    impl = key_impl( params->key );
    if (!impl->gcm || impl->gcm->flipped) return STATUS_INTERNAL_ERROR;  /* GnuTLS: no AEAD, or too late */
    br_gcm_aad_inject( &impl->gcm->ctx, params->auth_data, params->len );
    return STATUS_SUCCESS;
}

static NTSTATUS gcm_run( struct gcm *gcm, BOOL encrypt, const UCHAR *input, ULONG input_len,
                         UCHAR *output, ULONG output_len )
{
    if (output_len < input_len) return STATUS_INTERNAL_ERROR;
    if (!gcm->flipped)
    {
        br_gcm_flip( &gcm->ctx );
        gcm->flipped = TRUE;
    }
    if (output != input) memmove( output, input, input_len );
    br_gcm_run( &gcm->ctx, encrypt, output, input_len );
    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_encrypt( void *args )
{
    const struct key_symmetric_encrypt_params *params = args;
    struct key_impl *impl;
    NTSTATUS status;

    if ((status = init_cipher_handle( params->key ))) return status;

    impl = key_impl( params->key );
    if (impl->gcm)
        return gcm_run( impl->gcm, TRUE, params->input, params->input_len, params->output, params->output_len );
    if (params->output_len < params->input_len) return STATUS_INTERNAL_ERROR;
    return sym_run( params->key, TRUE, params->input, params->input_len, params->output );
}

static NTSTATUS key_symmetric_decrypt( void *args )
{
    const struct key_symmetric_decrypt_params *params = args;
    struct key_impl *impl;
    NTSTATUS status;

    if ((status = init_cipher_handle( params->key ))) return status;

    impl = key_impl( params->key );
    if (impl->gcm)
        return gcm_run( impl->gcm, FALSE, params->input, params->input_len, params->output, params->output_len );
    if (params->output_len < params->input_len) return STATUS_INTERNAL_ERROR;
    return sym_run( params->key, FALSE, params->input, params->input_len, params->output );
}

static NTSTATUS key_symmetric_get_tag( void *args )
{
    const struct key_symmetric_get_tag_params *params = args;
    struct key_impl *impl;
    br_gcm_context done;
    UCHAR tag[16];
    NTSTATUS status;

    if ((status = init_cipher_handle( params->key ))) return status;

    impl = key_impl( params->key );
    if (!impl->gcm || params->len > sizeof(tag)) return STATUS_INTERNAL_ERROR;   /* GnuTLS: no AEAD */
    if (!impl->gcm->flipped)
    {
        br_gcm_flip( &impl->gcm->ctx );
        impl->gcm->flipped = TRUE;
    }
    /* br_gcm_get_tag finishes the GHASH in the context: work on a copy so the tag can be asked
     * again, as GnuTLS allowed (the copy points at the same AES keys, which it only reads) */
    done = impl->gcm->ctx;
    br_gcm_get_tag( &done, tag );
    memcpy( params->tag, tag, params->len );
    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_destroy( void *args )
{
    struct key *key = args;
    struct key_impl *impl = key_impl( key );

    if (!impl) return STATUS_SUCCESS;
    key_symmetric_vector_reset( key );
    free( impl );
    key->private[0] = 0;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- exporting byte strings */

/* upstream's export_gnutls_datum: the value right-aligned in buflen bytes when zero_pad, else as
 * long as it is; returns the size written (or that would be written when buffer is NULL) */
static ULONG export_bytes( UCHAR *buffer, ULONG buflen, const struct bytes *d, BOOL zero_pad )
{
    const UCHAR *src = d->data;
    ULONG size = d->len, offset = 0;

    while (size > buflen && src && !*src) { src++; size--; }
    if (size > buflen)
    {
        ERR( "value of %u bytes doesn't fit in %u\n", (unsigned)size, (unsigned)buflen );
        src += size - buflen;
        size = buflen;
    }
    if (zero_pad)
    {
        offset = buflen - size;
        if (buffer) memset( buffer, 0, offset );
        if (buffer && size) memcpy( buffer + offset, src, size );
        return buflen;
    }
    if (buffer && size) memcpy( buffer, src, size );
    return size;
}

#define EXPORT_SIZE(d,l,p) export_bytes( NULL, l, &d, p )

static ULONG minimal_len( const struct bytes *b )
{
    ULONG len = b->len;
    const UCHAR *p = b->data;
    while (len && !*p) { p++; len--; }
    return len;
}

static NTSTATUS key_export_rsa_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
    struct key_impl *impl = key_impl( key );
    ULONG size = len_from_bitlen( key->u.a.bitlen );
    UCHAR *dst;

    if (!impl || !impl->pub) return STATUS_INVALID_PARAMETER;

    *ret_len = sizeof(*rsa_blob) + EXPORT_SIZE( impl->e, size, 0 ) + EXPORT_SIZE( impl->n, size, 1 );
    if (len >= *ret_len && buf)
    {
        dst = (UCHAR *)(rsa_blob + 1);
        rsa_blob->cbPublicExp = export_bytes( dst, size, &impl->e, 0 );

        dst += rsa_blob->cbPublicExp;
        rsa_blob->cbModulus = export_bytes( dst, size, &impl->n, 1 );

        rsa_blob->Magic     = BCRYPT_RSAPUBLIC_MAGIC;
        rsa_blob->BitLength = key->u.a.bitlen;
        rsa_blob->cbPrime1  = 0;
        rsa_blob->cbPrime2  = 0;
    }
    return STATUS_SUCCESS;
}

/* the blob magic and coordinate size of an ECC key; STATUS_NOT_IMPLEMENTED as upstream */
static NTSTATUS ecc_shape( const struct key *key, BOOL private, DWORD *magic, DWORD *size )
{
    switch (key->alg_id)
    {
    case ALG_ID_ECDH:
        switch (key->u.a.curve_id)
        {
        case ECC_CURVE_25519:
            *magic = private ? BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC : BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC;
            *size = 32;
            return STATUS_SUCCESS;
        case ECC_CURVE_P256R1:
            *magic = private ? BCRYPT_ECDH_PRIVATE_P256_MAGIC : BCRYPT_ECDH_PUBLIC_P256_MAGIC;
            *size = 32;
            return STATUS_SUCCESS;
        case ECC_CURVE_P384R1:
            *magic = private ? BCRYPT_ECDH_PRIVATE_P384_MAGIC : BCRYPT_ECDH_PUBLIC_P384_MAGIC;
            *size = 48;
            return STATUS_SUCCESS;
        case ECC_CURVE_P521R1:
            *magic = private ? BCRYPT_ECDH_PRIVATE_P521_MAGIC : BCRYPT_ECDH_PUBLIC_P521_MAGIC;
            *size = 66;
            return STATUS_SUCCESS;
        default:
            FIXME( "unsupported curve %u\n", key->u.a.curve_id );
            return STATUS_NOT_IMPLEMENTED;
        }

    case ALG_ID_ECDH_P256:
        *magic = private ? BCRYPT_ECDH_PRIVATE_P256_MAGIC : BCRYPT_ECDH_PUBLIC_P256_MAGIC;
        *size = 32;
        return STATUS_SUCCESS;

    case ALG_ID_ECDH_P384:
        *magic = private ? BCRYPT_ECDH_PRIVATE_P384_MAGIC : BCRYPT_ECDH_PUBLIC_P384_MAGIC;
        *size = 48;
        return STATUS_SUCCESS;

    case ALG_ID_ECDH_P521:
        *magic = private ? BCRYPT_ECDH_PRIVATE_P521_MAGIC : BCRYPT_ECDH_PUBLIC_P521_MAGIC;
        *size = 66;
        return STATUS_SUCCESS;

    case ALG_ID_ECDSA:
        switch (key->u.a.curve_id)
        {
        case ECC_CURVE_P256R1:
            *magic = private ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
            *size = 32;
            return STATUS_SUCCESS;
        case ECC_CURVE_P384R1:
            *magic = private ? BCRYPT_ECDSA_PRIVATE_P384_MAGIC : BCRYPT_ECDSA_PUBLIC_P384_MAGIC;
            *size = 48;
            return STATUS_SUCCESS;
        case ECC_CURVE_P521R1:
            *magic = private ? BCRYPT_ECDSA_PRIVATE_P521_MAGIC : BCRYPT_ECDSA_PUBLIC_P521_MAGIC;
            *size = 66;
            return STATUS_SUCCESS;
        default:
            FIXME( "unsupported curve %u\n", key->u.a.curve_id );
            return STATUS_NOT_IMPLEMENTED;
        }

    case ALG_ID_ECDSA_P256:
        *magic = private ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        *size = 32;
        return STATUS_SUCCESS;

    case ALG_ID_ECDSA_P384:
        *magic = private ? BCRYPT_ECDSA_PRIVATE_P384_MAGIC : BCRYPT_ECDSA_PUBLIC_P384_MAGIC;
        *size = 48;
        return STATUS_SUCCESS;

    case ALG_ID_ECDSA_P521:
        *magic = private ? BCRYPT_ECDSA_PRIVATE_P521_MAGIC : BCRYPT_ECDSA_PUBLIC_P521_MAGIC;
        *size = 66;
        return STATUS_SUCCESS;

    default:
        FIXME( "algorithm %u not supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

static BOOL is_x25519( const struct key *key )
{
    return key->alg_id == ALG_ID_ECDH && key->u.a.curve_id == ECC_CURVE_25519;
}

static NTSTATUS key_export_ecc_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
    struct key_impl *impl = key_impl( key );
    DWORD magic, size;
    NTSTATUS status;
    UCHAR *dst;

    if ((status = ecc_shape( key, FALSE, &magic, &size ))) return status;
    if (!impl || !impl->pub) return STATUS_INVALID_PARAMETER;

    *ret_len = sizeof(*ecc_blob) + EXPORT_SIZE( impl->ec_x, size, 1 ) + EXPORT_SIZE( impl->ec_y, size, 1 );
    if (len >= *ret_len && buf)
    {
        ecc_blob->dwMagic = magic;
        ecc_blob->cbKey   = size;

        dst = (UCHAR *)(ecc_blob + 1);
        dst += export_bytes( dst, size, &impl->ec_x, 1 );
        export_bytes( dst, size, &impl->ec_y, 1 );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_dsa_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_DSA_KEY_BLOB *dsa_blob = (BCRYPT_DSA_KEY_BLOB *)buf;
    struct key_impl *impl = key_impl( key );
    ULONG size = len_from_bitlen( key->u.a.bitlen );
    UCHAR *dst;

    if (key->u.a.bitlen > 1024)
    {
        FIXME( "bitlen > 1024 not supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!impl || !impl->pub) return STATUS_INVALID_PARAMETER;

    if (minimal_len( &impl->dsa_q ) > sizeof(dsa_blob->q)) return STATUS_INVALID_PARAMETER;

    *ret_len = sizeof(*dsa_blob) + EXPORT_SIZE( impl->dsa_p, size, 1 ) + EXPORT_SIZE( impl->dsa_g, size, 1 ) +
               EXPORT_SIZE( impl->dsa_y, size, 1 );
    if (len >= *ret_len && buf)
    {
        dst = (UCHAR *)(dsa_blob + 1);
        dst += export_bytes( dst, size, &impl->dsa_p, 1 );
        dst += export_bytes( dst, size, &impl->dsa_g, 1 );
        export_bytes( dst, size, &impl->dsa_y, 1 );

        dst = dsa_blob->q;
        export_bytes( dst, sizeof(dsa_blob->q), &impl->dsa_q, 1 );

        dsa_blob->dwMagic = BCRYPT_DSA_PUBLIC_MAGIC;
        dsa_blob->cbKey   = size;
        memset( dsa_blob->Count, 0, sizeof(dsa_blob->Count) ); /* FIXME */
        memset( dsa_blob->Seed, 0, sizeof(dsa_blob->Seed) ); /* FIXME */
    }
    return STATUS_SUCCESS;
}

static void reverse_bytes( UCHAR *buf, ULONG len )
{
    unsigned int i;
    UCHAR tmp;

    for (i = 0; i < len / 2; ++i)
    {
        tmp = buf[i];
        buf[i] = buf[len - i - 1];
        buf[len - i - 1] = tmp;
    }
}

#define Q_SIZE 20
static NTSTATUS key_export_dsa_capi_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BLOBHEADER *hdr = (BLOBHEADER *)buf;
    struct key_impl *impl = key_impl( key );
    DSSPUBKEY *dsskey;
    ULONG size = len_from_bitlen( key->u.a.bitlen );
    UCHAR *dst;

    if (key->u.a.bitlen > 1024)
    {
        FIXME( "bitlen > 1024 not supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!impl || (!impl->pub && !impl->priv)) return STATUS_INVALID_PARAMETER;

    if (minimal_len( &impl->dsa_q ) > Q_SIZE) return STATUS_INVALID_PARAMETER;

    *ret_len = sizeof(*hdr) + sizeof(*dsskey) + sizeof(key->u.a.dss_seed) +
               EXPORT_SIZE( impl->dsa_p, size, 1 ) + Q_SIZE + EXPORT_SIZE( impl->dsa_g, size, 1 ) +
               EXPORT_SIZE( impl->dsa_y, size, 1 );
    if (len >= *ret_len && buf)
    {
        hdr->bType    = PUBLICKEYBLOB;
        hdr->bVersion = 2;
        hdr->reserved = 0;
        hdr->aiKeyAlg = CALG_DSS_SIGN;

        dsskey = (DSSPUBKEY *)(hdr + 1);
        dsskey->magic  = MAGIC_DSS1;
        dsskey->bitlen = key->u.a.bitlen;

        dst = (UCHAR *)(dsskey + 1);
        export_bytes( dst, size, &impl->dsa_p, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_bytes( dst, Q_SIZE, &impl->dsa_q, 1 );
        reverse_bytes( dst, Q_SIZE );
        dst += Q_SIZE;

        export_bytes( dst, size, &impl->dsa_g, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_bytes( dst, size, &impl->dsa_y, 1 );
        reverse_bytes( dst, size );
        dst += size;

        memcpy( dst, &key->u.a.dss_seed, sizeof(key->u.a.dss_seed) );
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- SecKey objects from the material */

/* ANSI X9.63: 04 || X || Y [|| D] */
static UCHAR *ec_x963( struct key_impl *impl, BOOL private, ULONG *len )
{
    ULONG size = impl->ec_x.len;
    UCHAR *buf;

    *len = 1 + 2 * size + (private ? size : 0);
    if (!(buf = malloc( *len ))) return NULL;
    buf[0] = 4;
    memcpy( buf + 1, impl->ec_x.data, size );
    memcpy( buf + 1 + size, impl->ec_y.data, size );
    if (private) memcpy( buf + 1 + 2 * size, impl->ec_d.data, size );
    return buf;
}

static void rsa_parts( struct key_impl *impl, struct ios_rsa_parts *parts )
{
    memset( parts, 0, sizeof(*parts) );
    parts->n.data = impl->n.data;       parts->n.len = impl->n.len;
    parts->e.data = impl->e.data;       parts->e.len = impl->e.len;
    parts->d.data = impl->d.data;       parts->d.len = impl->d.len;
    parts->p.data = impl->p.data;       parts->p.len = impl->p.len;
    parts->q.data = impl->q.data;       parts->q.len = impl->q.len;
    parts->dp.data = impl->dp.data;     parts->dp.len = impl->dp.len;
    parts->dq.data = impl->dq.data;     parts->dq.len = impl->dq.len;
    parts->qinv.data = impl->qinv.data; parts->qinv.len = impl->qinv.len;
}

static ios_ac_key *get_sec_key( struct key *key, BOOL private )
{
    struct key_impl *impl = key_impl( key );
    ios_ac_key **slot;
    struct ios_rsa_parts parts;
    unsigned char *data = NULL;
    size_t len = 0;
    ULONG ulen;
    int ret = IOS_AC_FAIL;

    if (!impl || (private ? !impl->priv : !impl->pub)) return NULL;
    slot = private ? &impl->sec_priv : &impl->sec_pub;
    if (*slot) return *slot;

    switch (key->alg_id)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        rsa_parts( impl, &parts );
        if (private ? ios_der_rsa_private( &parts, &data, &len ) : ios_der_rsa_public( &parts, &data, &len )) return NULL;
        ret = ios_ac_key_create( IOS_AC_KEY_RSA, private, data, len, slot );
        break;
    default:
        if (is_x25519( key ) || !impl->ec_x.len) return NULL;
        if (!(data = ec_x963( impl, private, &ulen ))) return NULL;
        ret = ios_ac_key_create( IOS_AC_KEY_EC, private, data, ulen, slot );
        break;
    }
    if (data) memset( data, 0, len ? len : 0 );
    free( data );
    return ret == IOS_AC_OK ? *slot : NULL;
}

/* ---------------------------------------------------------------- BearSSL helpers */

static BOOL seed_drbg( br_hmac_drbg_context *rng )
{
    unsigned char seed[32];

    if (ios_ac_random( seed, sizeof(seed) )) return FALSE;
    br_hmac_drbg_init( rng, &br_sha256_vtable, seed, sizeof(seed) );
    memset( seed, 0, sizeof(seed) );
    return TRUE;
}

static BOOL bear_rsa_keys( struct key *key, br_rsa_private_key *sk, br_rsa_public_key *pk )
{
    struct key_impl *impl = key_impl( key );

    if (key->u.a.bitlen > 4096 || !impl) return FALSE;
    if (pk)
    {
        if (!impl->pub) return FALSE;
        pk->n = impl->n.data; pk->nlen = impl->n.len;
        pk->e = impl->e.data; pk->elen = impl->e.len;
    }
    if (sk)
    {
        if (!impl->priv) return FALSE;
        sk->n_bitlen = key->u.a.bitlen;
        sk->p = impl->p.data;   sk->plen = impl->p.len;
        sk->q = impl->q.data;   sk->qlen = impl->q.len;
        sk->dp = impl->dp.data; sk->dplen = impl->dp.len;
        sk->dq = impl->dq.data; sk->dqlen = impl->dq.len;
        sk->iq = impl->qinv.data; sk->iqlen = impl->qinv.len;
    }
    return TRUE;
}

/* ---------------------------------------------------------------- generate */

static void log_once( int *flag, const char *msg )
{
    if (!__atomic_exchange_n( flag, 1, __ATOMIC_RELAXED )) fprintf( stderr, "[bcrypt-ios] %s\n", msg );
}

static NTSTATUS rsa_from_der( struct key_impl *impl, const unsigned char *der, size_t len )
{
    struct ios_rsa_parts parts;
    int is_private;

    if (ios_der_rsa_parse( der, len, &parts, &is_private ) || !is_private) return STATUS_INTERNAL_ERROR;
    if (!bytes_set( &impl->n, parts.n.data, parts.n.len, TRUE ) || !bytes_set( &impl->e, parts.e.data, parts.e.len, TRUE ) ||
        !bytes_set( &impl->d, parts.d.data, parts.d.len, TRUE ) || !bytes_set( &impl->p, parts.p.data, parts.p.len, TRUE ) ||
        !bytes_set( &impl->q, parts.q.data, parts.q.len, TRUE ) || !bytes_set( &impl->dp, parts.dp.data, parts.dp.len, TRUE ) ||
        !bytes_set( &impl->dq, parts.dq.data, parts.dq.len, TRUE ) ||
        !bytes_set( &impl->qinv, parts.qinv.data, parts.qinv.len, TRUE ))
        return STATUS_NO_MEMORY;
    impl->priv = impl->pub = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS generate_rsa( struct key *key, struct key_impl *impl )
{
    static int fallback_logged;
    ios_ac_key *sec;
    unsigned char *der;
    size_t len;
    NTSTATUS status;
    int ret;

    if ((ret = ios_ac_key_generate( IOS_AC_KEY_RSA, key->u.a.bitlen, &sec )) == IOS_AC_OK)
    {
        if (ios_ac_key_export( sec, &der, &len ))
        {
            ios_ac_key_release( sec );
            return STATUS_INTERNAL_ERROR;
        }
        status = rsa_from_der( impl, der, len );
        memset( der, 0, len );
        free( der );
        if (status) { ios_ac_key_release( sec ); return status; }
        impl->sec_priv = sec;
        return STATUS_SUCCESS;
    }
    if (ret != IOS_AC_UNSUPPORTED) return STATUS_INTERNAL_ERROR;

    /* Security refused the size: BearSSL, e = 65537, 512-4096 bits */
    if (key->u.a.bitlen < 512 || key->u.a.bitlen > 4096) return STATUS_NOT_SUPPORTED;
    log_once( &fallback_logged, "RSA key size refused by Security.framework: generating with BearSSL" );
    {
        unsigned char *kpriv = malloc( BR_RSA_KBUF_PRIV_SIZE(4096) ), *kpub = malloc( BR_RSA_KBUF_PUB_SIZE(4096) );
        unsigned char *d = malloc( 512 );
        br_hmac_drbg_context rng;
        br_rsa_private_key sk;
        br_rsa_public_key pk;
        size_t dlen;

        status = STATUS_INTERNAL_ERROR;
        if (kpriv && kpub && d && seed_drbg( &rng ) &&
            br_rsa_keygen_get_default()( &rng.vtable, &sk, kpriv, &pk, kpub, key->u.a.bitlen, 65537 ) &&
            (dlen = br_rsa_compute_privexp_get_default()( d, &sk, 65537 )))
        {
            if (bytes_set( &impl->n, pk.n, pk.nlen, TRUE ) && bytes_set( &impl->e, pk.e, pk.elen, TRUE ) &&
                bytes_set( &impl->d, d, dlen, TRUE ) && bytes_set( &impl->p, sk.p, sk.plen, TRUE ) &&
                bytes_set( &impl->q, sk.q, sk.qlen, TRUE ) && bytes_set( &impl->dp, sk.dp, sk.dplen, TRUE ) &&
                bytes_set( &impl->dq, sk.dq, sk.dqlen, TRUE ) && bytes_set( &impl->qinv, sk.iq, sk.iqlen, TRUE ))
            {
                impl->priv = impl->pub = TRUE;
                status = STATUS_SUCCESS;
            }
        }
        if (kpriv) memset( kpriv, 0, BR_RSA_KBUF_PRIV_SIZE(4096) );
        if (d) memset( d, 0, 512 );
        free( kpriv ); free( kpub ); free( d );
        return status;
    }
}

static unsigned int curve_bits( const struct key *key )
{
    switch (key->alg_id)
    {
    case ALG_ID_ECDH_P256: case ALG_ID_ECDSA_P256: return 256;
    case ALG_ID_ECDH_P384: case ALG_ID_ECDSA_P384: return 384;
    case ALG_ID_ECDH_P521: case ALG_ID_ECDSA_P521: return 521;
    case ALG_ID_ECDH: case ALG_ID_ECDSA:
        switch (key->u.a.curve_id)
        {
        case ECC_CURVE_P256R1: return 256;
        case ECC_CURVE_P384R1: return 384;
        case ECC_CURVE_P521R1: return 521;
        default: return 0;
        }
    default: return 0;
    }
}

static ULONG coord_size( unsigned int bits ) { return (bits + 7) / 8; }

/* the material of an EC key from Apple's X9.63 export */
static NTSTATUS ec_from_x963( struct key_impl *impl, const unsigned char *data, size_t len, ULONG size, BOOL private )
{
    if (len != 1 + size * (private ? 3 : 2) || data[0] != 4) return STATUS_INTERNAL_ERROR;
    if (!bytes_set( &impl->ec_x, data + 1, size, FALSE ) || !bytes_set( &impl->ec_y, data + 1 + size, size, FALSE ))
        return STATUS_NO_MEMORY;
    if (private && !bytes_set( &impl->ec_d, data + 1 + 2 * size, size, FALSE )) return STATUS_NO_MEMORY;
    impl->pub = TRUE;
    if (private) impl->priv = TRUE;
    return STATUS_SUCCESS;
}

/* X25519 (RFC 7748): private = 32 random bytes, public = d * 9. BearSSL's mul takes the scalar
 * big-endian, so the RFC's little-endian string is reversed for it. */
static BOOL x25519( const UCHAR *d, const UCHAR *point, UCHAR *out )
{
    const br_ec_impl *ec = &br_ec_c25519_m31;
    UCHAR k[32];
    unsigned int i;
    uint32_t ok;

    for (i = 0; i < 32; i++) k[i] = d[31 - i];
    if (point)
    {
        memcpy( out, point, 32 );
        ok = ec->mul( out, 32, k, 32, BR_EC_curve25519 );
    }
    else ok = ec->mulgen( out, k, 32, BR_EC_curve25519 ) == 32;
    memset( k, 0, sizeof(k) );
    return ok != 0;
}

static NTSTATUS generate_ec( struct key *key, struct key_impl *impl )
{
    ios_ac_key *sec;
    unsigned char *data;
    size_t len;
    unsigned int bits;
    NTSTATUS status;

    if (is_x25519( key ))
    {
        UCHAR d[32], x[32];

        if (ios_ac_random( d, sizeof(d) ) || !x25519( d, NULL, x )) return STATUS_INTERNAL_ERROR;
        if (!bytes_set( &impl->ec_d, d, 32, FALSE ) || !bytes_set( &impl->ec_x, x, 32, FALSE )) return STATUS_NO_MEMORY;
        bytes_free( &impl->ec_y );
        memset( d, 0, sizeof(d) );
        impl->priv = impl->pub = TRUE;
        return STATUS_SUCCESS;
    }
    if (!(bits = curve_bits( key ))) return STATUS_INVALID_PARAMETER;
    if (ios_ac_key_generate( IOS_AC_KEY_EC, bits, &sec )) return STATUS_INTERNAL_ERROR;
    if (ios_ac_key_export( sec, &data, &len ))
    {
        ios_ac_key_release( sec );
        return STATUS_INTERNAL_ERROR;
    }
    status = ec_from_x963( impl, data, len, coord_size( bits ), TRUE );
    memset( data, 0, len );
    free( data );
    if (status) { ios_ac_key_release( sec ); return status; }
    impl->sec_priv = sec;
    return STATUS_SUCCESS;
}

static NTSTATUS generate_dh( struct key *key, struct key_impl *impl )
{
    struct ios_buf p, g, y, x;
    int from_group;

    if (!impl->params)
    {
        /* no parameters set: the MODP group of that size, else fresh ones (clarification 8.4) */
        if (ios_ltc_dh_params( key->u.a.bitlen, &from_group, &p, &g )) return STATUS_INTERNAL_ERROR;
        bytes_take( &impl->dh_p, &p );
        bytes_take( &impl->dh_g, &g );
        impl->params = TRUE;
    }
    if (ios_ltc_dh_generate( impl->dh_p.data, impl->dh_p.len, impl->dh_g.data, impl->dh_g.len, &y, &x ))
        return STATUS_INTERNAL_ERROR;
    bytes_take( &impl->dh_y, &y );
    bytes_take( &impl->dh_x, &x );
    impl->priv = impl->pub = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS generate_dsa( struct key *key, struct key_impl *impl )
{
    struct ios_buf p, q, g, y, x;

    if (key->u.a.bitlen < 512 || key->u.a.bitlen > 1024 || key->u.a.bitlen % 64) return STATUS_NOT_SUPPORTED;
    if (ios_ltc_dsa_generate( key->u.a.bitlen, &p, &q, &g, &y, &x )) return STATUS_INTERNAL_ERROR;
    bytes_take( &impl->dsa_p, &p );
    bytes_take( &impl->dsa_q, &q );
    bytes_take( &impl->dsa_g, &g );
    bytes_take( &impl->dsa_y, &y );
    bytes_take( &impl->dsa_x, &x );
    impl->priv = impl->pub = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_generate( void *args )
{
    struct key *key = args;
    struct key_impl *impl = key_impl( key );

    if (impl && impl->priv) return STATUS_INVALID_HANDLE;

    switch (key->alg_id)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
    case ALG_ID_DH:
    case ALG_ID_DSA:
        break;

    case ALG_ID_ECDH:
    case ALG_ID_ECDSA:
        switch (key->u.a.curve_id)
        {
        case ECC_CURVE_25519:
            assert( key->alg_id == ALG_ID_ECDH );
            break;
        case ECC_CURVE_P256R1:
        case ECC_CURVE_P384R1:
        case ECC_CURVE_P521R1:
            break;
        default:
            FIXME( "unsupported ECDH/ECDSA curve %u\n", key->u.a.curve_id );
            return STATUS_INVALID_PARAMETER;
        }
        break;

    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P384:
    case ALG_ID_ECDH_P521:
    case ALG_ID_ECDSA_P521:
        break;

    default:
        FIXME( "algorithm %u not supported\n", key->alg_id );
        return STATUS_NOT_SUPPORTED;
    }

    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;
    switch (key->alg_id)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN: return generate_rsa( key, impl );
    case ALG_ID_DH:       return generate_dh( key, impl );
    case ALG_ID_DSA:      return generate_dsa( key, impl );
    default:              return generate_ec( key, impl );
    }
}

/* ---------------------------------------------------------------- export, import */

static NTSTATUS key_export_ecc( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob;
    struct key_impl *impl = key_impl( key );
    DWORD magic, size;
    NTSTATUS status;
    UCHAR *dst;

    if ((status = ecc_shape( key, TRUE, &magic, &size )))
    {
        if (status == STATUS_NOT_IMPLEMENTED && key->alg_id != ALG_ID_ECDH && key->alg_id != ALG_ID_ECDSA)
            FIXME( "algorithm %u does not yet support exporting ecc blob\n", key->alg_id );
        return status;
    }

    if (!impl || !impl->priv) return STATUS_INVALID_PARAMETER;

    *ret_len = sizeof(*ecc_blob) + EXPORT_SIZE( impl->ec_x, size, 1 ) + EXPORT_SIZE( impl->ec_y, size, 1 ) +
               EXPORT_SIZE( impl->ec_d, size, 1 );
    if (len >= *ret_len && buf)
    {
        ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
        ecc_blob->dwMagic = magic;
        ecc_blob->cbKey   = size;

        dst = (UCHAR *)(ecc_blob + 1);
        dst += export_bytes( dst, size, &impl->ec_x, 1 );
        dst += export_bytes( dst, size, &impl->ec_y, 1 );
        export_bytes( dst, size, &impl->ec_d, 1 );
    }
    return STATUS_SUCCESS;
}

/* the curve's coordinate size for an ECC import; STATUS_NOT_IMPLEMENTED as upstream */
static NTSTATUS ecc_import_size( const struct key *key, ULONG *size )
{
    switch (key->alg_id)
    {
    case ALG_ID_ECDH:
        switch (key->u.a.curve_id)
        {
        case ECC_CURVE_25519:  *size = 32; return STATUS_SUCCESS;
        case ECC_CURVE_P256R1: *size = 32; return STATUS_SUCCESS;
        case ECC_CURVE_P384R1: *size = 48; return STATUS_SUCCESS;
        case ECC_CURVE_P521R1: *size = 66; return STATUS_SUCCESS;
        default:
            FIXME( "curve %u not supported\n", key->u.a.curve_id );
            return STATUS_NOT_IMPLEMENTED;
        }

    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDSA_P256:
        *size = 32;
        return STATUS_SUCCESS;

    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P384:
        *size = 48;
        return STATUS_SUCCESS;

    case ALG_ID_ECDH_P521:
    case ALG_ID_ECDSA_P521:
        *size = 66;
        return STATUS_SUCCESS;

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

static NTSTATUS key_import_ecc( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
    struct key_impl *impl;
    UCHAR *x, *y, *d;
    ULONG size;
    NTSTATUS status;

    if ((status = ecc_import_size( key, &size ))) return status;
    if (ecc_blob->cbKey != size) return STATUS_INTERNAL_ERROR;          /* GnuTLS refused the values */
    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    x = (UCHAR *)(ecc_blob + 1);
    y = x + ecc_blob->cbKey;
    d = y + ecc_blob->cbKey;

    clear_asymmetric( impl );
    if (!bytes_set( &impl->ec_x, x, size, FALSE ) || !bytes_set( &impl->ec_d, d, size, FALSE ) ||
        (!is_x25519( key ) && !bytes_set( &impl->ec_y, y, size, FALSE )))
        return STATUS_NO_MEMORY;
    impl->priv = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_rsa( struct key *key, ULONG flags, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob;
    struct key_impl *impl = key_impl( key );
    ULONG size = len_from_bitlen( key->u.a.bitlen );
    BOOL full = (flags & KEY_EXPORT_FLAG_RSA_FULL);
    UCHAR *dst;

    if (!impl || !impl->priv) return STATUS_INVALID_PARAMETER;

    *ret_len = sizeof(*rsa_blob) + EXPORT_SIZE( impl->e, size, 0 ) + EXPORT_SIZE( impl->n, size, 1 ) +
               EXPORT_SIZE( impl->p, size / 2, 1 ) + EXPORT_SIZE( impl->q, size / 2, 1 );

    if (full) *ret_len += EXPORT_SIZE( impl->dp, size / 2, 1 ) + EXPORT_SIZE( impl->dq, size / 2, 1 ) +
                          EXPORT_SIZE( impl->qinv, size / 2, 1 ) + EXPORT_SIZE( impl->d, size, 1 );

    if (len >= *ret_len && buf)
    {
        rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
        rsa_blob->Magic     = full ? BCRYPT_RSAFULLPRIVATE_MAGIC : BCRYPT_RSAPRIVATE_MAGIC;
        rsa_blob->BitLength = key->u.a.bitlen;

        dst = (UCHAR *)(rsa_blob + 1);
        rsa_blob->cbPublicExp = export_bytes( dst, size, &impl->e, 0 );

        dst += rsa_blob->cbPublicExp;
        rsa_blob->cbModulus = export_bytes( dst, size, &impl->n, 1 );

        dst += rsa_blob->cbModulus;
        rsa_blob->cbPrime1 = export_bytes( dst, size / 2, &impl->p, 1 );

        dst += rsa_blob->cbPrime1;
        rsa_blob->cbPrime2 = export_bytes( dst, size / 2, &impl->q, 1 );

        if (full)
        {
            dst += rsa_blob->cbPrime2;
            export_bytes( dst, size / 2, &impl->dp, 1 );

            dst += rsa_blob->cbPrime1;
            export_bytes( dst, size / 2, &impl->dq, 1 );

            dst += rsa_blob->cbPrime2;
            export_bytes( dst, size / 2, &impl->qinv, 1 );

            dst += rsa_blob->cbPrime1;
            export_bytes( dst, size, &impl->d, 1 );
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_rsa( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
    struct key_impl *impl;
    UCHAR *e, *m, *p, *q;
    struct ios_buf d, dp, dq, qinv;

    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    e = (UCHAR *)(rsa_blob + 1);
    m = e + rsa_blob->cbPublicExp;
    p = m + rsa_blob->cbModulus;
    q = p + rsa_blob->cbPrime1;

    clear_asymmetric( impl );
    if (!bytes_set( &impl->e, e, rsa_blob->cbPublicExp, TRUE ) || !bytes_set( &impl->n, m, rsa_blob->cbModulus, TRUE ) ||
        !bytes_set( &impl->p, p, rsa_blob->cbPrime1, TRUE ) || !bytes_set( &impl->q, q, rsa_blob->cbPrime2, TRUE ))
        return STATUS_NO_MEMORY;
    if (!impl->e.len || !impl->n.len || !impl->p.len || !impl->q.len) return STATUS_INTERNAL_ERROR;

    if (rsa_blob->Magic == BCRYPT_RSAFULLPRIVATE_MAGIC)
    {
        /* the material as given: dp, dq, qinv, d follow the primes */
        UCHAR *pdp = q + rsa_blob->cbPrime2, *pdq = pdp + rsa_blob->cbPrime1;
        UCHAR *pqi = pdq + rsa_blob->cbPrime2, *pd = pqi + rsa_blob->cbPrime1;

        if (!bytes_set( &impl->dp, pdp, rsa_blob->cbPrime1, TRUE ) || !bytes_set( &impl->dq, pdq, rsa_blob->cbPrime2, TRUE ) ||
            !bytes_set( &impl->qinv, pqi, rsa_blob->cbPrime1, TRUE ) || !bytes_set( &impl->d, pd, rsa_blob->cbModulus, TRUE ))
            return STATUS_NO_MEMORY;
    }
    else
    {
        /* RSAPRIVATE (n, e, p, q): complete d, dp, dq, qinv with LibTomMath */
        if (ios_ltc_rsa_complete( impl->e.data, impl->e.len, impl->p.data, impl->p.len, impl->q.data, impl->q.len,
                                  &d, &dp, &dq, &qinv ))
            return STATUS_INTERNAL_ERROR;
        bytes_take( &impl->d, &d );
        bytes_take( &impl->dp, &dp );
        bytes_take( &impl->dq, &dq );
        bytes_take( &impl->qinv, &qinv );
    }
    impl->priv = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_dsa_capi( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BLOBHEADER *hdr;
    DSSPUBKEY *pubkey;
    struct key_impl *impl = key_impl( key );
    ULONG size = len_from_bitlen( key->u.a.bitlen );
    UCHAR *dst;

    if (!impl || !impl->priv) return STATUS_INVALID_PARAMETER;

    if (minimal_len( &impl->dsa_q ) > 20 || minimal_len( &impl->dsa_x ) > 20)
    {
        ERR( "can't export key in this format\n" );
        return STATUS_NOT_SUPPORTED;
    }

    *ret_len = sizeof(*hdr) + sizeof(*pubkey) + sizeof(key->u.a.dss_seed) +
               EXPORT_SIZE( impl->dsa_p, size, 1 ) + 20 + EXPORT_SIZE( impl->dsa_g, size, 1 ) + 20;
    if (len >= *ret_len && buf)
    {
        hdr = (BLOBHEADER *)buf;
        hdr->bType    = PRIVATEKEYBLOB;
        hdr->bVersion = 2;
        hdr->reserved = 0;
        hdr->aiKeyAlg = CALG_DSS_SIGN;

        pubkey = (DSSPUBKEY *)(hdr + 1);
        pubkey->magic  = MAGIC_DSS2;
        pubkey->bitlen = key->u.a.bitlen;

        dst = (UCHAR *)(pubkey + 1);
        export_bytes( dst, size, &impl->dsa_p, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_bytes( dst, 20, &impl->dsa_q, 1 );
        reverse_bytes( dst, 20 );
        dst += 20;

        export_bytes( dst, size, &impl->dsa_g, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_bytes( dst, 20, &impl->dsa_x, 1 );
        reverse_bytes( dst, 20 );
        dst += 20;

        memcpy( dst, &key->u.a.dss_seed, sizeof(key->u.a.dss_seed) );
    }
    return STATUS_SUCCESS;
}

/* little-endian CAPI value -> big-endian copy */
static BOOL set_reversed( struct bytes *b, const UCHAR *data, ULONG len )
{
    UCHAR tmp[512];
    ULONG i;

    if (len > sizeof(tmp)) return FALSE;
    for (i = 0; i < len; i++) tmp[i] = data[len - i - 1];
    return bytes_set( b, tmp, len, TRUE );
}

static NTSTATUS key_import_dsa_capi( struct key *key, UCHAR *buf, ULONG len )
{
    BLOBHEADER *hdr = (BLOBHEADER *)buf;
    DSSPUBKEY *pubkey;
    struct key_impl *impl;
    unsigned char *data;
    ULONG size;

    pubkey = (DSSPUBKEY *)(hdr + 1);
    if ((size = len_from_bitlen( pubkey->bitlen )) > 128)
    {
        FIXME( "size %u not supported\n", (unsigned)size );
        return STATUS_NOT_SUPPORTED;
    }
    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;
    data = (unsigned char *)(pubkey + 1);

    clear_asymmetric( impl );
    if (!set_reversed( &impl->dsa_p, data, size )) return STATUS_NO_MEMORY;
    data += size;
    if (!set_reversed( &impl->dsa_q, data, 20 )) return STATUS_NO_MEMORY;
    data += 20;
    if (!set_reversed( &impl->dsa_g, data, size )) return STATUS_NO_MEMORY;
    data += size;
    if (!set_reversed( &impl->dsa_x, data, 20 )) return STATUS_NO_MEMORY;
    data += 20;

    memcpy( &key->u.a.dss_seed, data, sizeof(key->u.a.dss_seed) );
    impl->priv = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_ecc_public( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
    struct key_impl *impl;
    ULONG size;
    NTSTATUS status;

    if ((status = ecc_import_size( key, &size ))) return status;
    if (ecc_blob->cbKey != size) return STATUS_INTERNAL_ERROR;
    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    clear_asymmetric( impl );
    if (!bytes_set( &impl->ec_x, buf + sizeof(*ecc_blob), size, FALSE ) ||
        (!is_x25519( key ) && !bytes_set( &impl->ec_y, buf + sizeof(*ecc_blob) + size, size, FALSE )))
        return STATUS_NO_MEMORY;
    impl->pub = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_rsa_public( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
    struct key_impl *impl;

    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    clear_asymmetric( impl );
    if (!bytes_set( &impl->e, buf + sizeof(*rsa_blob), rsa_blob->cbPublicExp, TRUE ) ||
        !bytes_set( &impl->n, buf + sizeof(*rsa_blob) + rsa_blob->cbPublicExp, rsa_blob->cbModulus, TRUE ))
        return STATUS_NO_MEMORY;
    if (!impl->e.len || !impl->n.len) return STATUS_INTERNAL_ERROR;
    impl->pub = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_dsa_public( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_DSA_KEY_BLOB *dsa_blob = (BCRYPT_DSA_KEY_BLOB *)buf;
    struct key_impl *impl;

    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    clear_asymmetric( impl );
    if (!bytes_set( &impl->dsa_p, buf + sizeof(*dsa_blob), dsa_blob->cbKey, TRUE ) ||
        !bytes_set( &impl->dsa_q, dsa_blob->q, sizeof(dsa_blob->q), TRUE ) ||
        !bytes_set( &impl->dsa_g, buf + sizeof(*dsa_blob) + dsa_blob->cbKey, dsa_blob->cbKey, TRUE ) ||
        !bytes_set( &impl->dsa_y, buf + sizeof(*dsa_blob) + dsa_blob->cbKey * 2, dsa_blob->cbKey, TRUE ))
        return STATUS_NO_MEMORY;
    impl->pub = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_dsa_capi_public( struct key *key, UCHAR *buf, ULONG len )
{
    BLOBHEADER *hdr = (BLOBHEADER *)buf;
    DSSPUBKEY *pubkey = (DSSPUBKEY *)(hdr + 1);
    struct key_impl *impl;
    unsigned char *data;
    ULONG size = len_from_bitlen( pubkey->bitlen );

    if (size > 128) return STATUS_INTERNAL_ERROR;
    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;
    data = (unsigned char *)(pubkey + 1);

    clear_asymmetric( impl );
    if (!set_reversed( &impl->dsa_p, data, size )) return STATUS_NO_MEMORY;
    data += size;
    if (!set_reversed( &impl->dsa_q, data, 20 )) return STATUS_NO_MEMORY;
    data += 20;
    if (!set_reversed( &impl->dsa_g, data, size )) return STATUS_NO_MEMORY;
    data += size;
    /* y has the key's size (upstream read 128 bytes whatever the size: a listed difference) */
    if (!set_reversed( &impl->dsa_y, data, size )) return STATUS_NO_MEMORY;
    impl->pub = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_dh_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_DH_KEY_BLOB *dh_blob = (BCRYPT_DH_KEY_BLOB *)buf;
    struct key_impl *impl = key_impl( key );
    ULONG size = len_from_bitlen( key->u.a.bitlen );
    UCHAR *dst;

    if (!impl || !impl->pub || !impl->params) return STATUS_INTERNAL_ERROR;

    *ret_len = sizeof(*dh_blob) + EXPORT_SIZE(impl->dh_p, size, 1) + EXPORT_SIZE(impl->dh_g, size, 1) +
               EXPORT_SIZE(impl->dh_y, size, 1);
    if (len >= *ret_len && buf)
    {
        dst = (UCHAR *)(dh_blob + 1);
        dst += export_bytes( dst, size, &impl->dh_p, 1 );
        dst += export_bytes( dst, size, &impl->dh_g, 1 );
        dst += export_bytes( dst, size, &impl->dh_y, 1 );

        dh_blob->dwMagic = BCRYPT_DH_PUBLIC_MAGIC;
        dh_blob->cbKey   = size;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_dh( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_DH_KEY_BLOB *dh_blob = (BCRYPT_DH_KEY_BLOB *)buf;
    struct key_impl *impl = key_impl( key );
    ULONG size = len_from_bitlen( key->u.a.bitlen );
    UCHAR *dst;

    if (!impl || !impl->priv) return STATUS_INVALID_PARAMETER;
    if (!impl->params) return STATUS_INTERNAL_ERROR;

    *ret_len = sizeof(*dh_blob) + EXPORT_SIZE(impl->dh_p, size, 1) + EXPORT_SIZE(impl->dh_g, size, 1) +
                                  EXPORT_SIZE(impl->dh_y, size, 1) + EXPORT_SIZE(impl->dh_x, size, 1);
    if (len >= *ret_len && buf)
    {
        dst = (UCHAR *)(dh_blob + 1);
        dst += export_bytes( dst, size, &impl->dh_p, 1 );
        dst += export_bytes( dst, size, &impl->dh_g, 1 );
        dst += export_bytes( dst, size, &impl->dh_y, 1 );
        dst += export_bytes( dst, size, &impl->dh_x, 1 );

        dh_blob->dwMagic = BCRYPT_DH_PRIVATE_MAGIC;
        dh_blob->cbKey   = size;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_dh_params( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_DH_PARAMETER_HEADER *hdr = (BCRYPT_DH_PARAMETER_HEADER *)buf;
    struct key_impl *impl = key_impl( key );
    unsigned int size = sizeof(*hdr) + len_from_bitlen( key->u.a.bitlen ) * 2;
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR *dst;

    if (!impl || !impl->params) return STATUS_INVALID_PARAMETER;

    *ret_len = size;
    if (len < size) status = STATUS_BUFFER_TOO_SMALL;
    else if (buf)
    {
        hdr->cbLength    = size;
        hdr->dwMagic     = BCRYPT_DH_PARAMETERS_MAGIC;
        hdr->cbKeyLength = len_from_bitlen( key->u.a.bitlen );

        dst = (UCHAR *)(hdr + 1);
        dst += export_bytes( dst, hdr->cbKeyLength, &impl->dh_p, 1 );
        dst += export_bytes( dst, hdr->cbKeyLength, &impl->dh_g, 1 );
    }
    return status;
}

static NTSTATUS key_asymmetric_export( void *args )
{
    const struct key_asymmetric_export_params *params = args;
    struct key *key = params->key;
    unsigned flags = params->flags;

    if (!(key->u.a.flags & KEY_FLAG_FINALIZED)) return STATUS_INVALID_HANDLE;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH:
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDH_P521:
    case ALG_ID_ECDSA:
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    case ALG_ID_ECDSA_P521:
        if (flags & KEY_EXPORT_FLAG_PUBLIC)
            return key_export_ecc_public( key, params->buf, params->len, params->ret_len );
        return key_export_ecc( key, params->buf, params->len, params->ret_len );

    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        if (flags & KEY_EXPORT_FLAG_PUBLIC)
            return key_export_rsa_public( key, params->buf, params->len, params->ret_len );
        return key_export_rsa( key, flags, params->buf, params->len, params->ret_len );

    case ALG_ID_DSA:
        if (flags & KEY_EXPORT_FLAG_PUBLIC)
        {
            if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
                return key_export_dsa_capi_public( key, params->buf, params->len, params->ret_len );
            return key_export_dsa_public( key, params->buf, params->len, params->ret_len );
        }
        if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
            return key_export_dsa_capi( key, params->buf, params->len, params->ret_len );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_DH:
        if (flags & KEY_EXPORT_FLAG_DH_PARAMETERS)
            return key_export_dh_params( key, params->buf, params->len, params->ret_len );
        if (flags & KEY_EXPORT_FLAG_PUBLIC)
            return key_export_dh_public( key, params->buf, params->len, params->ret_len );
        return key_export_dh( key, params->buf, params->len, params->ret_len );

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

static NTSTATUS key_import_dh_public( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_DH_KEY_BLOB *dh_blob = (BCRYPT_DH_KEY_BLOB *)buf;
    struct key_impl *impl;

    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    clear_asymmetric( impl );
    if (!bytes_set( &impl->dh_p, buf + sizeof(*dh_blob), dh_blob->cbKey, TRUE ) ||
        !bytes_set( &impl->dh_g, buf + sizeof(*dh_blob) + dh_blob->cbKey, dh_blob->cbKey, TRUE ) ||
        !bytes_set( &impl->dh_y, buf + sizeof(*dh_blob) + dh_blob->cbKey * 2, dh_blob->cbKey, TRUE ))
        return STATUS_NO_MEMORY;
    if (!impl->dh_p.len || !impl->dh_g.len) return STATUS_INTERNAL_ERROR;
    impl->pub = impl->params = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_dh( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_DH_KEY_BLOB *dh_blob = (BCRYPT_DH_KEY_BLOB *)buf;
    struct key_impl *impl;

    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    clear_asymmetric( impl );
    if (!bytes_set( &impl->dh_p, buf + sizeof(*dh_blob), dh_blob->cbKey, TRUE ) ||
        !bytes_set( &impl->dh_g, buf + sizeof(*dh_blob) + dh_blob->cbKey, dh_blob->cbKey, TRUE ) ||
        !bytes_set( &impl->dh_y, buf + sizeof(*dh_blob) + dh_blob->cbKey * 2, dh_blob->cbKey, TRUE ) ||
        !bytes_set( &impl->dh_x, buf + sizeof(*dh_blob) + dh_blob->cbKey * 3, dh_blob->cbKey, TRUE ))
        return STATUS_NO_MEMORY;
    if (!impl->dh_p.len || !impl->dh_g.len) return STATUS_INTERNAL_ERROR;
    impl->priv = impl->params = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_dh_params( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_DH_PARAMETER_HEADER *dh_header = (BCRYPT_DH_PARAMETER_HEADER *)buf;
    struct key_impl *impl;
    UCHAR *p = (UCHAR *)(dh_header + 1);

    if (!(impl = key_impl_get( key ))) return STATUS_NO_MEMORY;

    if (!bytes_set( &impl->dh_p, p, dh_header->cbKeyLength, TRUE ) ||
        !bytes_set( &impl->dh_g, p + dh_header->cbKeyLength, dh_header->cbKeyLength, TRUE ))
        return STATUS_NO_MEMORY;
    if (!impl->dh_p.len || !impl->dh_g.len) return STATUS_INTERNAL_ERROR;
    impl->params = TRUE;
    return STATUS_SUCCESS;
}

/* after a private import, the public half as GnuTLS's pubkey_import_privkey made it; a key whose
 * public half can't be had stays private only, as upstream */
static void derive_public( struct key *key, struct key_impl *impl )
{
    struct ios_buf y;
    UCHAR x[32];

    switch (key->alg_id)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
    case ALG_ID_DH:
        impl->pub = TRUE;
        break;
    case ALG_ID_DSA:
        if (!ios_ltc_dsa_public( impl->dsa_p.data, impl->dsa_p.len, impl->dsa_q.data, impl->dsa_q.len,
                                 impl->dsa_g.data, impl->dsa_g.len, impl->dsa_x.data, impl->dsa_x.len, &y ))
        {
            bytes_take( &impl->dsa_y, &y );
            impl->pub = TRUE;
        }
        break;
    default:
        if (is_x25519( key ))
        {
            /* the given x is the public key; an all-zero one is computed from d */
            static const UCHAR zero[32];
            if (impl->ec_x.len == 32 && !memcmp( impl->ec_x.data, zero, 32 ) && x25519( impl->ec_d.data, NULL, x ))
                bytes_set( &impl->ec_x, x, 32, FALSE );
        }
        impl->pub = TRUE;
        break;
    }
}

static NTSTATUS key_asymmetric_import( void *args )
{
    const struct key_asymmetric_import_params *params = args;
    struct key *key = params->key;
    unsigned flags = params->flags;
    NTSTATUS ret;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH:
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDH_P521:
    case ALG_ID_ECDSA:
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    case ALG_ID_ECDSA_P521:
        if (flags & KEY_IMPORT_FLAG_PUBLIC)
            return key_import_ecc_public( key, params->buf, params->len );
        ret = key_import_ecc( key, params->buf, params->len );
        break;

    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        if (flags & KEY_IMPORT_FLAG_PUBLIC)
            return key_import_rsa_public( key, params->buf, params->len );
        ret = key_import_rsa( key, params->buf, params->len );
        break;

    case ALG_ID_DSA:
        if (flags & KEY_IMPORT_FLAG_PUBLIC)
        {
            if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
                return key_import_dsa_capi_public( key, params->buf, params->len );
            return key_import_dsa_public( key, params->buf, params->len );
        }
        if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
        {
            ret = key_import_dsa_capi( key, params->buf, params->len );
            break;
        }
        FIXME( "DSA private key not supported\n" );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_DH:
        if (flags & KEY_IMPORT_FLAG_DH_PARAMETERS)
            return key_import_dh_params( key, params->buf, params->len );
        if (flags & KEY_IMPORT_FLAG_PUBLIC)
            return key_import_dh_public( key, params->buf, params->len );
        ret = key_import_dh( key, params->buf, params->len );
        break;

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (ret) return ret;

    derive_public( key, key_impl( key ) );
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- hashes of the padding infos */

enum digest { DIG_UNKNOWN, DIG_SHA1, DIG_SHA256, DIG_SHA384, DIG_SHA512, DIG_MD2, DIG_MD5 };

static enum digest get_digest_from_id( const WCHAR *alg_id )
{
    if (!wcscmp( alg_id, BCRYPT_SHA1_ALGORITHM ))   return DIG_SHA1;
    if (!wcscmp( alg_id, BCRYPT_SHA256_ALGORITHM )) return DIG_SHA256;
    if (!wcscmp( alg_id, BCRYPT_SHA384_ALGORITHM )) return DIG_SHA384;
    if (!wcscmp( alg_id, BCRYPT_SHA512_ALGORITHM )) return DIG_SHA512;
    if (!wcscmp( alg_id, BCRYPT_MD2_ALGORITHM ))    return DIG_MD2;
    if (!wcscmp( alg_id, BCRYPT_MD5_ALGORITHM ))    return DIG_MD5;
    return DIG_UNKNOWN;
}

static ULONG digest_len( enum digest d )
{
    switch (d)
    {
    case DIG_SHA1:   return 20;
    case DIG_SHA256: return 32;
    case DIG_SHA384: return 48;
    case DIG_SHA512: return 64;
    case DIG_MD2: case DIG_MD5: return 16;
    default: return 0;
    }
}

static const br_hash_class *bear_hash( enum digest d )
{
    switch (d)
    {
    case DIG_SHA1:   return &br_sha1_vtable;
    case DIG_SHA256: return &br_sha256_vtable;
    case DIG_SHA384: return &br_sha384_vtable;
    case DIG_SHA512: return &br_sha512_vtable;
    case DIG_MD5:    return &br_md5_vtable;
    default:         return NULL;
    }
}

static enum ios_ac_alg sec_alg( enum digest d, enum ios_ac_alg sha1 )
{
    return (enum ios_ac_alg)(sha1 + (d - DIG_SHA1));
}

/* RFC 8017 section 9.2 note 1: the DigestInfo prefixes of MD2 and MD5 */
static const UCHAR digestinfo_md2[] =
    { 0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x02, 0x05, 0x00, 0x04, 0x10 };
static const UCHAR digestinfo_md5[] =
    { 0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00, 0x04, 0x10 };

/* the bytes an RSA PKCS #1 v1.5 "raw" signature covers: DigestInfo || hash for MD2/MD5, else the
 * input as given */
static UCHAR *pkcs1_raw_input( enum digest d, const UCHAR *hash, ULONG len, ULONG *ret_len )
{
    const UCHAR *prefix = d == DIG_MD2 ? digestinfo_md2 : d == DIG_MD5 ? digestinfo_md5 : NULL;
    ULONG plen = prefix ? sizeof(digestinfo_md5) : 0;
    UCHAR *buf;

    if (!(buf = malloc( plen + len + 1 ))) return NULL;
    if (plen) memcpy( buf, prefix, plen );
    memcpy( buf + plen, hash, len );
    *ret_len = plen + len;
    return buf;
}

/* ---------------------------------------------------------------- verify */

static NTSTATUS verify_rsa_pkcs1( struct key *key, enum digest d, BOOL raw, const UCHAR *hash, ULONG hash_len,
                                  const UCHAR *sig, ULONG sig_len )
{
    ios_ac_key *pub = get_sec_key( key, FALSE );
    UCHAR *input;
    ULONG input_len;
    int ret;

    if (!pub) return STATUS_INVALID_SIGNATURE;
    if (!raw && d >= DIG_SHA1 && d <= DIG_SHA512)
        ret = ios_ac_verify( pub, sec_alg( d, IOS_AC_RSA_SIG_PKCS1_SHA1 ), hash, hash_len, sig, sig_len );
    else
    {
        if (!(input = pkcs1_raw_input( raw ? DIG_UNKNOWN : d, hash, hash_len, &input_len ))) return STATUS_NO_MEMORY;
        ret = ios_ac_verify( pub, IOS_AC_RSA_SIG_PKCS1_RAW, input, input_len, sig, sig_len );
        free( input );
    }
    return ret ? STATUS_INVALID_SIGNATURE : STATUS_SUCCESS;
}

static NTSTATUS verify_rsa_pss( struct key *key, enum digest d, ULONG salt, const UCHAR *hash, ULONG hash_len,
                                const UCHAR *sig, ULONG sig_len )
{
    if (d >= DIG_SHA1 && d <= DIG_SHA512 && salt == digest_len( d ))
    {
        ios_ac_key *pub = get_sec_key( key, FALSE );
        if (!pub) return STATUS_INVALID_SIGNATURE;
        return ios_ac_verify( pub, sec_alg( d, IOS_AC_RSA_SIG_PSS_SHA1 ), hash, hash_len, sig, sig_len )
               ? STATUS_INVALID_SIGNATURE : STATUS_SUCCESS;
    }
    if (d == DIG_MD2) return STATUS_NOT_SUPPORTED;
    {
        br_rsa_public_key pk;
        if (!bear_rsa_keys( key, NULL, &pk ))
            return key->u.a.bitlen > 4096 ? STATUS_NOT_SUPPORTED : STATUS_INVALID_SIGNATURE;
        if (hash_len != digest_len( d )) return STATUS_INVALID_SIGNATURE;
        return br_rsa_pss_vrfy_get_default()( sig, sig_len, bear_hash( d ), bear_hash( d ), hash, salt, &pk )
               ? STATUS_SUCCESS : STATUS_INVALID_SIGNATURE;
    }
}

static NTSTATUS key_asymmetric_verify( void *args )
{
    const struct key_asymmetric_verify_params *params = args;
    struct key *key = params->key;
    struct key_impl *impl = key_impl( key );
    unsigned flags = params->flags;
    enum digest d;

    switch (key->alg_id)
    {
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    case ALG_ID_ECDSA_P521:
    {
        struct buffer buffer;
        ios_ac_key *pub;
        UCHAR *r, *s;
        ULONG half, r_len, s_len;
        int ret;

        if (flags) FIXME( "flags %#x not supported\n", flags );

        /* only the hash size must match, not the actual hash function */
        switch (params->hash_len)
        {
        case 20: case 32: case 48: break;
        default:
            FIXME( "hash size %u not yet supported\n", params->hash_len );
            return STATUS_INVALID_SIGNATURE;
        }
        if (!(pub = get_sec_key( key, FALSE ))) return STATUS_INVALID_SIGNATURE;
        if (!(half = params->signature_len / 2)) return STATUS_INVALID_SIGNATURE;

        /* r || s -> DER, as upstream prepared it for GnuTLS, but with minimal INTEGERs: Security
         * wants DER, and a half with a leading zero byte would not be */
        r = params->signature;
        s = params->signature + half;
        r_len = s_len = half;
        while (r_len > 1 && !*r) { r++; r_len--; }
        while (s_len > 1 && !*s) { s++; s_len--; }
        buffer_init( &buffer );
        buffer_append_asn1_r_s( &buffer, r, r_len, s, s_len );
        if (buffer.error)
        {
            buffer_free( &buffer );
            return STATUS_NO_MEMORY;
        }
        ret = ios_ac_verify( pub, IOS_AC_ECDSA_DIGEST_X962, params->hash, params->hash_len, buffer.buffer, buffer.pos );
        buffer_free( &buffer );
        return ret ? STATUS_INVALID_SIGNATURE : STATUS_SUCCESS;
    }
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
    {
        if (flags & BCRYPT_PAD_PKCS1)
        {
            BCRYPT_PKCS1_PADDING_INFO *info = params->padding;

            if (!info) return STATUS_INVALID_PARAMETER;
            if (!info->pszAlgId)
                return verify_rsa_pkcs1( key, DIG_UNKNOWN, TRUE, params->hash, params->hash_len,
                                         params->signature, params->signature_len );
            if ((d = get_digest_from_id(info->pszAlgId)) == DIG_UNKNOWN)
            {
                FIXME( "hash algorithm %s not supported\n", debugstr_w(info->pszAlgId) );
                return STATUS_NOT_SUPPORTED;
            }
            return verify_rsa_pkcs1( key, d, FALSE, params->hash, params->hash_len,
                                     params->signature, params->signature_len );
        }
        else if (flags & BCRYPT_PAD_PSS)
        {
            BCRYPT_PSS_PADDING_INFO *info = params->padding;

            if (!info) return STATUS_INVALID_PARAMETER;
            if (!info->pszAlgId) return STATUS_INVALID_SIGNATURE;
            if ((d = get_digest_from_id(info->pszAlgId)) == DIG_UNKNOWN)
            {
                FIXME( "hash algorithm %s not supported\n", debugstr_w(info->pszAlgId) );
                return STATUS_NOT_SUPPORTED;
            }
            return verify_rsa_pss( key, d, info->cbSalt, params->hash, params->hash_len,
                                   params->signature, params->signature_len );
        }
        else return STATUS_INVALID_PARAMETER;
    }
    case ALG_ID_DSA:
    {
        int ret;

        if (flags) FIXME( "flags %#x not supported\n", flags );
        if (params->hash_len != 20)
        {
            FIXME( "hash size %u not supported\n", params->hash_len );
            return STATUS_INVALID_PARAMETER;
        }
        if (!impl || !impl->pub) return STATUS_INVALID_SIGNATURE;
        ret = ios_ltc_dsa_verify( impl->dsa_p.data, impl->dsa_p.len, impl->dsa_q.data, impl->dsa_q.len,
                                  impl->dsa_g.data, impl->dsa_g.len, impl->dsa_y.data, impl->dsa_y.len,
                                  params->hash, params->hash_len, params->signature, params->signature_len / 2,
                                  params->signature + params->signature_len / 2, params->signature_len / 2 );
        return ret == 1 ? STATUS_SUCCESS : STATUS_INVALID_SIGNATURE;
    }
    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

/* ---------------------------------------------------------------- sign */

static unsigned int get_signature_length( enum alg_id id )
{
    switch (id)
    {
    case ALG_ID_ECDSA_P256: return 64;
    case ALG_ID_ECDSA_P384: return 96;
    case ALG_ID_ECDSA_P521: return 132;
    case ALG_ID_DSA:        return 40;
    default:
        FIXME( "unhandled algorithm %u\n", id );
        return 0;
    }
}

/* an RSA signature into the output, upstream's format_gnutls_signature for RSA */
static NTSTATUS output_rsa_signature( const UCHAR *sig, ULONG sig_len, UCHAR *output, ULONG output_len, ULONG *ret_len )
{
    *ret_len = sig_len;
    if (output_len < sig_len) return STATUS_BUFFER_TOO_SMALL;
    if (output) memcpy( output, sig, sig_len );
    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_sign( void *args )
{
    const struct key_asymmetric_sign_params *params = args;
    struct key *key = params->key;
    struct key_impl *impl = key_impl( key );
    unsigned int flags = params->flags;
    enum digest hash_alg = DIG_UNKNOWN;
    BOOL pss = FALSE, raw = FALSE;
    ULONG salt = 0;
    unsigned char *sig = NULL;
    size_t sig_len = 0;
    NTSTATUS status;

    if (key->alg_id == ALG_ID_ECDSA_P256 || key->alg_id == ALG_ID_ECDSA_P384 || key->alg_id == ALG_ID_ECDSA_P521)
    {
        /* With ECDSA, we find the digest algorithm from the hash length, and verify it */
        switch (params->input_len)
        {
        case 20: hash_alg = DIG_SHA1; break;
        case 32: hash_alg = DIG_SHA256; break;
        case 48: hash_alg = DIG_SHA384; break;
        case 64: hash_alg = DIG_SHA512; break;

        default:
            FIXME( "hash size %u not yet supported\n", params->input_len );
            return STATUS_INVALID_PARAMETER;
        }

        if (flags == BCRYPT_PAD_PKCS1)
        {
            BCRYPT_PKCS1_PADDING_INFO *pad = params->padding;
            if (pad && pad->pszAlgId && get_digest_from_id( pad->pszAlgId ) != hash_alg)
            {
                WARN( "incorrect hashing algorithm %s, expected %u\n", debugstr_w(pad->pszAlgId), hash_alg );
                return STATUS_INVALID_PARAMETER;
            }
        }
    }
    else if (key->alg_id == ALG_ID_DSA)
    {
        if (flags) FIXME( "flags %#x not supported\n", flags );
        if (params->input_len != 20)
        {
            FIXME( "hash size %u not supported\n", params->input_len );
            return STATUS_INVALID_PARAMETER;
        }
        hash_alg = DIG_SHA1;
    }
    else if (flags == BCRYPT_PAD_PKCS1)
    {
        BCRYPT_PKCS1_PADDING_INFO *pad = params->padding;

        if (!pad)
        {
            WARN( "padding info not found\n" );
            return STATUS_INVALID_PARAMETER;
        }
        if (!pad->pszAlgId) raw = TRUE;
        else if ((hash_alg = get_digest_from_id( pad->pszAlgId )) == DIG_UNKNOWN)
        {
            FIXME( "hash algorithm %s not recognized\n", debugstr_w(pad->pszAlgId) );
            return STATUS_NOT_SUPPORTED;
        }
    }
    else if (flags == BCRYPT_PAD_PSS)
    {
        BCRYPT_PSS_PADDING_INFO *pad = params->padding;

        if (!pad || !pad->pszAlgId)
        {
            WARN( "padding info not found\n" );
            return STATUS_INVALID_PARAMETER;
        }
        if (key->alg_id != ALG_ID_RSA && key->alg_id != ALG_ID_RSA_SIGN)
        {
            FIXME( "BCRYPT_PAD_PSS not supported for key algorithm %u\n", key->alg_id );
            return STATUS_NOT_SUPPORTED;
        }
        if ((hash_alg = get_digest_from_id( pad->pszAlgId )) == DIG_UNKNOWN)
        {
            FIXME( "hash algorithm %s not recognized\n", debugstr_w(pad->pszAlgId) );
            return STATUS_NOT_SUPPORTED;
        }
        if (hash_alg == DIG_MD2) return STATUS_NOT_SUPPORTED;
        pss = TRUE;
        salt = pad->cbSalt;
    }
    else if (!flags)
    {
         WARN( "invalid flags %#x\n", flags );
         return STATUS_INVALID_PARAMETER;
    }
    else
    {
        FIXME( "flags %#x not implemented\n", flags );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!params->output)
    {
        *params->ret_len = len_from_bitlen( key->u.a.bitlen );
        return STATUS_SUCCESS;
    }
    if (!impl || !impl->priv) return STATUS_INVALID_PARAMETER;

    switch (key->alg_id)
    {
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    case ALG_ID_ECDSA_P521:
    {
        unsigned int sig_size = get_signature_length( key->alg_id );
        UCHAR rs[132];
        ios_ac_key *priv = get_sec_key( key, TRUE );

        if (!priv || ios_ac_sign( priv, IOS_AC_ECDSA_DIGEST_X962, params->input, params->input_len, &sig, &sig_len ))
            return STATUS_INTERNAL_ERROR;
        /* DER -> r || s, each half left-padded (upstream's format_gnutls_signature) */
        *params->ret_len = sig_size;
        if (params->output_len < sig_size) status = STATUS_BUFFER_TOO_SMALL;
        else if (ios_der_sig_to_rs( sig, sig_len, rs, sig_size / 2 ))
        {
            ERR( "we didn't get a correct signature\n" );
            status = STATUS_INTERNAL_ERROR;
        }
        else
        {
            memcpy( params->output, rs, sig_size );
            status = STATUS_SUCCESS;
        }
        free( sig );
        return status;
    }
    case ALG_ID_DSA:
    {
        UCHAR r[20], s[20];

        if (ios_ltc_dsa_sign( impl->dsa_p.data, impl->dsa_p.len, impl->dsa_q.data, impl->dsa_q.len,
                              impl->dsa_g.data, impl->dsa_g.len, impl->dsa_x.data, impl->dsa_x.len,
                              params->input, params->input_len, r, s ))
            return STATUS_INTERNAL_ERROR;
        *params->ret_len = 40;
        if (params->output_len < 40) return STATUS_BUFFER_TOO_SMALL;
        memcpy( params->output, r, 20 );
        memcpy( params->output + 20, s, 20 );
        return STATUS_SUCCESS;
    }
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        break;
    default:
        return STATUS_INTERNAL_ERROR;
    }

    /* RSA */
    if (!pss && !raw && hash_alg >= DIG_SHA1 && hash_alg <= DIG_SHA512)
    {
        ios_ac_key *priv = get_sec_key( key, TRUE );
        if (!priv || ios_ac_sign( priv, sec_alg( hash_alg, IOS_AC_RSA_SIG_PKCS1_SHA1 ), params->input,
                                  params->input_len, &sig, &sig_len ))
            return STATUS_INTERNAL_ERROR;
    }
    else if (!pss)
    {
        /* MD5 or MD2 over DigestInfo, or the input as given (no algorithm id) */
        ios_ac_key *priv = get_sec_key( key, TRUE );
        UCHAR *input;
        ULONG input_len;

        if (!priv) return STATUS_INTERNAL_ERROR;
        if (!(input = pkcs1_raw_input( raw ? DIG_UNKNOWN : hash_alg, params->input, params->input_len, &input_len )))
            return STATUS_NO_MEMORY;
        status = ios_ac_sign( priv, IOS_AC_RSA_SIG_PKCS1_RAW, input, input_len, &sig, &sig_len ) ? STATUS_INTERNAL_ERROR : 0;
        free( input );
        if (status) return status;
    }
    else if (hash_alg >= DIG_SHA1 && hash_alg <= DIG_SHA512 && salt == digest_len( hash_alg ))
    {
        ios_ac_key *priv = get_sec_key( key, TRUE );
        if (!priv || ios_ac_sign( priv, sec_alg( hash_alg, IOS_AC_RSA_SIG_PSS_SHA1 ), params->input,
                                  params->input_len, &sig, &sig_len ))
            return STATUS_INTERNAL_ERROR;
    }
    else
    {
        /* PSS with another salt length, or MD5: BearSSL */
        br_hmac_drbg_context rng;
        br_rsa_private_key sk;

        if (!bear_rsa_keys( key, &sk, NULL )) return STATUS_NOT_SUPPORTED;
        if (params->input_len != digest_len( hash_alg )) return STATUS_INTERNAL_ERROR;
        sig_len = len_from_bitlen( key->u.a.bitlen );
        if (!(sig = malloc( sig_len ))) return STATUS_NO_MEMORY;
        if (!seed_drbg( &rng ) ||
            !br_rsa_pss_sign_get_default()( &rng.vtable, bear_hash( hash_alg ), bear_hash( hash_alg ),
                                            params->input, salt, &sk, sig ))
        {
            free( sig );
            return STATUS_INTERNAL_ERROR;
        }
    }

    status = output_rsa_signature( sig, sig_len, params->output, params->output_len, params->ret_len );
    free( sig );
    return status;
}

/* ---------------------------------------------------------------- destroy, duplicate */

static NTSTATUS key_asymmetric_destroy( void *args )
{
    struct key *key = args;
    struct key_impl *impl = key_impl( key );

    if (!impl) return STATUS_SUCCESS;
    clear_asymmetric( impl );
    free( impl );
    key->private[0] = 0;
    return STATUS_SUCCESS;
}

static BOOL copy_bytes( struct bytes *dst, const struct bytes *src )
{
    if (!src->data) { bytes_free( dst ); return TRUE; }
    return bytes_set( dst, src->data, src->len, FALSE );
}

static NTSTATUS key_asymmetric_duplicate( void *args )
{
    const struct key_asymmetric_duplicate_params *params = args;
    struct key_impl *orig = key_impl( params->key_orig ), *copy;
    const struct bytes *src[] = { &orig->n, &orig->e, &orig->d, &orig->p, &orig->q, &orig->dp, &orig->dq, &orig->qinv,
                                  &orig->ec_x, &orig->ec_y, &orig->ec_d, &orig->dh_p, &orig->dh_g, &orig->dh_y,
                                  &orig->dh_x, &orig->dsa_p, &orig->dsa_q, &orig->dsa_g, &orig->dsa_y, &orig->dsa_x };
    struct bytes *dst[ARRAY_SIZE(src)];
    unsigned int i;

    if (!orig || (!orig->priv && !orig->pub)) return STATUS_SUCCESS;
    if (!(copy = key_impl_get( params->key_copy ))) return STATUS_INTERNAL_ERROR;

    dst[0] = &copy->n; dst[1] = &copy->e; dst[2] = &copy->d; dst[3] = &copy->p; dst[4] = &copy->q;
    dst[5] = &copy->dp; dst[6] = &copy->dq; dst[7] = &copy->qinv; dst[8] = &copy->ec_x; dst[9] = &copy->ec_y;
    dst[10] = &copy->ec_d; dst[11] = &copy->dh_p; dst[12] = &copy->dh_g; dst[13] = &copy->dh_y; dst[14] = &copy->dh_x;
    dst[15] = &copy->dsa_p; dst[16] = &copy->dsa_q; dst[17] = &copy->dsa_g; dst[18] = &copy->dsa_y; dst[19] = &copy->dsa_x;

    clear_asymmetric( copy );
    for (i = 0; i < ARRAY_SIZE(src); i++)
        if (!copy_bytes( dst[i], src[i] )) return STATUS_INTERNAL_ERROR;
    copy->priv = orig->priv;
    copy->pub = orig->pub;
    copy->params = orig->params;
    /* a SecKeyRef is immutable: shared */
    copy->sec_priv = ios_ac_key_retain( orig->sec_priv );
    copy->sec_pub = ios_ac_key_retain( orig->sec_pub );
    if (params->key_orig->alg_id == ALG_ID_DSA) params->key_copy->u.a.dss_seed = params->key_orig->u.a.dss_seed;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- RSA decrypt, encrypt */

/* the OAEP route: Security for SHA-1/2 without a label, BearSSL for a label or MD5 */
enum oaep_route { OAEP_SEC, OAEP_BEAR };

static NTSTATUS oaep_route( BCRYPT_OAEP_PADDING_INFO *pad, enum digest *d, enum oaep_route *route )
{
    if (!pad || !pad->pszAlgId)
    {
        WARN( "padding info not found\n" );
        return STATUS_INVALID_PARAMETER;
    }
    if ((*d = get_digest_from_id( pad->pszAlgId )) == DIG_UNKNOWN)
    {
        FIXME( "hash algorithm %s not recognized\n", debugstr_w(pad->pszAlgId) );
        return STATUS_NOT_SUPPORTED;
    }
    if (*d == DIG_MD2) return STATUS_NOT_SUPPORTED;
    *route = (pad->pbLabel && pad->cbLabel) || *d == DIG_MD5 ? OAEP_BEAR : OAEP_SEC;
    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_decrypt( void *args )
{
    const struct key_asymmetric_decrypt_params *params = args;
    struct key *key = params->key;
    NTSTATUS status = STATUS_SUCCESS;
    unsigned char *out = NULL;
    size_t out_len = 0;

    if (key->alg_id == ALG_ID_RSA && (params->flags & BCRYPT_PAD_OAEP))
    {
        BCRYPT_OAEP_PADDING_INFO *pad = params->padding;
        enum oaep_route route;
        enum digest d;

        if ((status = oaep_route( pad, &d, &route ))) return status;
        if (route == OAEP_BEAR)
        {
            br_rsa_private_key sk;

            if (!bear_rsa_keys( key, &sk, NULL ))
                return key->u.a.bitlen > 4096 ? STATUS_NOT_SUPPORTED : STATUS_INTERNAL_ERROR;
            if (!(out = malloc( params->input_len ? params->input_len : 1 ))) return STATUS_NO_MEMORY;
            memcpy( out, params->input, params->input_len );
            out_len = params->input_len;
            if (!br_rsa_oaep_decrypt_get_default()( bear_hash( d ), pad->pbLabel, pad->cbLabel, &sk, out, &out_len ))
            {
                free( out );
                return STATUS_INTERNAL_ERROR;
            }
        }
        else
        {
            ios_ac_key *priv = get_sec_key( key, TRUE );
            if (!priv || ios_ac_decrypt( priv, sec_alg( d, IOS_AC_RSA_ENC_OAEP_SHA1 ), params->input,
                                         params->input_len, &out, &out_len ))
                return STATUS_INTERNAL_ERROR;
        }
    }
    else
    {
        ios_ac_key *priv = get_sec_key( key, TRUE );
        enum ios_ac_alg alg = (params->flags & BCRYPT_PAD_NONE) ? IOS_AC_RSA_ENC_RAW : IOS_AC_RSA_ENC_PKCS1;

        if (!priv || ios_ac_decrypt( priv, alg, params->input, params->input_len, &out, &out_len ))
            return STATUS_INTERNAL_ERROR;
    }

    *params->ret_len = out_len;
    if (params->output_len >= out_len) memcpy( params->output, out, *params->ret_len );
    else if (params->output) status = STATUS_BUFFER_TOO_SMALL;

    memset( out, 0, out_len );
    free( out );
    return status;
}

static NTSTATUS key_asymmetric_encrypt( void *args )
{
    const struct key_asymmetric_encrypt_params *params = args;
    struct key *key = params->key;
    struct key_impl *impl = key_impl( key );
    NTSTATUS status = STATUS_SUCCESS;
    unsigned char *out = NULL;
    size_t out_len = 0;
    ULONG k = len_from_bitlen( key->u.a.bitlen );

    if (!impl || !impl->pub) return STATUS_INVALID_HANDLE;

    if (key->alg_id == ALG_ID_RSA && (!params->output || k > params->output_len))
    {
        *params->ret_len = k;
        return !params->output ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
    }

    if (key->alg_id == ALG_ID_RSA && (params->flags & BCRYPT_PAD_OAEP))
    {
        BCRYPT_OAEP_PADDING_INFO *pad = params->padding;
        enum oaep_route route;
        enum digest d;

        if ((status = oaep_route( pad, &d, &route ))) return status;
        if (route == OAEP_BEAR)
        {
            br_hmac_drbg_context rng;
            br_rsa_public_key pk;

            if (!bear_rsa_keys( key, NULL, &pk ))
                return key->u.a.bitlen > 4096 ? STATUS_NOT_SUPPORTED : STATUS_INTERNAL_ERROR;
            if (!(out = malloc( k ))) return STATUS_NO_MEMORY;
            if (!seed_drbg( &rng ) ||
                !(out_len = br_rsa_oaep_encrypt_get_default()( &rng.vtable, bear_hash( d ), pad->pbLabel, pad->cbLabel,
                                                               &pk, out, k, params->input, params->input_len )))
            {
                free( out );
                return STATUS_INTERNAL_ERROR;
            }
        }
        else
        {
            ios_ac_key *pub = get_sec_key( key, FALSE );
            if (!pub || ios_ac_encrypt( pub, sec_alg( d, IOS_AC_RSA_ENC_OAEP_SHA1 ), params->input,
                                        params->input_len, &out, &out_len ))
                return STATUS_INTERNAL_ERROR;
        }
    }
    else if (params->flags & BCRYPT_PAD_NONE)
    {
        /* raw RSA: the input left-padded with zeros to the modulus length, below the modulus */
        ios_ac_key *pub = get_sec_key( key, FALSE );
        UCHAR *block;

        if (params->input_len > k) return STATUS_INVALID_PARAMETER;
        if (!(block = calloc( 1, k ))) return STATUS_NO_MEMORY;
        memcpy( block + k - params->input_len, params->input, params->input_len );
        if (impl->n.len == k && memcmp( block, impl->n.data, k ) >= 0)
        {
            free( block );
            return STATUS_INVALID_PARAMETER;
        }
        if (!pub || ios_ac_encrypt( pub, IOS_AC_RSA_ENC_RAW, block, k, &out, &out_len ))
        {
            free( block );
            return STATUS_INTERNAL_ERROR;
        }
        free( block );
    }
    else
    {
        ios_ac_key *pub = get_sec_key( key, FALSE );
        if (!pub || ios_ac_encrypt( pub, IOS_AC_RSA_ENC_PKCS1, params->input, params->input_len, &out, &out_len ))
            return STATUS_INTERNAL_ERROR;
    }

    *params->ret_len = out_len;
    if (params->output_len >= out_len) memcpy( params->output, out, *params->ret_len );
    else if (params->output_len == 0) status = STATUS_SUCCESS;
    else status = STATUS_BUFFER_TOO_SMALL;

    free( out );
    return status;
}

/* ---------------------------------------------------------------- derive */

static NTSTATUS key_asymmetric_derive_key( void *args )
{
    const struct key_asymmetric_derive_key_params *params = args;
    struct key_impl *priv = key_impl( params->privkey ), *pub = key_impl( params->pubkey );
    struct bytes s = { NULL, 0 };
    NTSTATUS status = STATUS_SUCCESS;

    if (!priv || !priv->priv || !pub || !pub->pub) return STATUS_INTERNAL_ERROR;

    if (params->privkey->alg_id == ALG_ID_DH)
    {
        struct ios_buf secret;

        if (!priv->params || ios_ltc_dh_shared( priv->dh_p.data, priv->dh_p.len, priv->dh_g.data, priv->dh_g.len,
                                                priv->dh_x.data, priv->dh_x.len, pub->dh_y.data, pub->dh_y.len,
                                                &secret ))
            return STATUS_INTERNAL_ERROR;
        bytes_take( &s, &secret );
    }
    else if (is_x25519( params->privkey ))
    {
        static const UCHAR zero[32];
        UCHAR out[32];

        if (priv->ec_d.len != 32 || pub->ec_x.len != 32 || !x25519( priv->ec_d.data, pub->ec_x.data, out ) ||
            !memcmp( out, zero, 32 ))
            return STATUS_INTERNAL_ERROR;
        if (!bytes_set( &s, out, 32, FALSE )) return STATUS_NO_MEMORY;
        memset( out, 0, sizeof(out) );
    }
    else
    {
        ios_ac_key *sk = get_sec_key( params->privkey, TRUE ), *pk = get_sec_key( params->pubkey, FALSE );
        unsigned char *out;
        size_t out_len;

        if (!sk || !pk || ios_ac_ecdh( sk, pk, &out, &out_len )) return STATUS_INTERNAL_ERROR;
        s.data = out;
        s.len = out_len;
    }

    *params->ret_len = EXPORT_SIZE( s, len_from_bitlen( params->privkey->u.a.bitlen ), 1 );
    if (params->output)
    {
        if (params->output_len < *params->ret_len) status = STATUS_BUFFER_TOO_SMALL;
        else export_bytes( params->output, *params->ret_len, &s, 1 );
    }

    if (s.data) memset( s.data, 0, s.len );
    bytes_free( &s );
    return status;
}

/* ---------------------------------------------------------------- attach, self-test (clarifications 8.6, 13) */

static UCHAR *hex( const char *str, ULONG *len )
{
    ULONG n = strlen( str ) / 2, i;
    UCHAR *buf = malloc( n ? n : 1 );

    for (i = 0; buf && i < n; i++)
    {
        unsigned int v;
        sscanf( str + 2 * i, "%2x", &v );
        buf[i] = v;
    }
    *len = n;
    return buf;
}

static BOOL hex_equal( const UCHAR *data, ULONG len, const char *str )
{
    ULONG n;
    UCHAR *want = hex( str, &n );
    BOOL ret = want && n == len && !memcmp( data, want, n );
    free( want );
    return ret;
}

static void selftest_symmetric_key( struct key *key, enum chain_mode mode, UCHAR *secret, ULONG secret_len,
                                    UCHAR *iv, ULONG iv_len )
{
    memset( key, 0, sizeof(*key) );
    key->alg_id = ALG_ID_AES;
    key->u.s.mode = mode;
    key->u.s.block_size = 16;
    key->u.s.secret = secret;
    key->u.s.secret_len = secret_len;
    key->u.s.vector = iv;
    key->u.s.vector_len = iv_len;
}

/* SP 800-38A F.2.1 (CBC) and F.3.7 (CFB8) encryption with AES-128 */
static BOOL selftest_aes( enum chain_mode mode, const char *pt_hex, const char *ct_hex )
{
    struct key key;
    struct key_symmetric_encrypt_params p;
    ULONG klen, ivlen, ptlen;
    UCHAR *k = hex( "2b7e151628aed2a6abf7158809cf4f3c", &klen ), *iv = hex( "000102030405060708090a0b0c0d0e0f", &ivlen );
    UCHAR *pt = hex( pt_hex, &ptlen ), *ct = malloc( ptlen );
    BOOL ok = FALSE;

    if (k && iv && pt && ct)
    {
        selftest_symmetric_key( &key, mode, k, klen, iv, ivlen );
        p.key = &key; p.input = pt; p.input_len = ptlen; p.output = ct; p.output_len = ptlen;
        ok = !key_symmetric_encrypt( &p ) && hex_equal( ct, ptlen, ct_hex );
        key_symmetric_destroy( &key );
    }
    free( k ); free( iv ); free( pt ); free( ct );
    return ok;
}

/* GCM test case 4 (McGrew-Viega, SP 800-38D) */
static BOOL selftest_gcm( void )
{
    struct key key;
    struct key_symmetric_set_auth_data_params a;
    struct key_symmetric_encrypt_params e;
    struct key_symmetric_get_tag_params t;
    ULONG klen, ivlen, alen, plen;
    UCHAR *k = hex( "feffe9928665731c6d6a8f9467308308", &klen ), *iv = hex( "cafebabefacedbaddecaf888", &ivlen );
    UCHAR *aad = hex( "feedfacedeadbeeffeedfacedeadbeefabaddad2", &alen );
    UCHAR *pt = hex( "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525"
                     "b16aedf5aa0de657ba637b39", &plen );
    UCHAR *ct = malloc( plen ), tag[16];
    BOOL ok = FALSE;

    if (k && iv && aad && pt && ct)
    {
        selftest_symmetric_key( &key, CHAIN_MODE_GCM, k, klen, iv, ivlen );
        a.key = &key; a.auth_data = aad; a.len = alen;
        e.key = &key; e.input = pt; e.input_len = plen; e.output = ct; e.output_len = plen;
        t.key = &key; t.tag = tag; t.len = 16;
        ok = !key_symmetric_set_auth_data( &a ) && !key_symmetric_encrypt( &e ) && !key_symmetric_get_tag( &t ) &&
             hex_equal( ct, plen, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c"
                                  "7d8f6a5aac84aa051ba30b396a0aac973d58e091" ) &&
             hex_equal( tag, 16, "5bc94fbc3221a5db94fae95ae7121a47" );
        key_symmetric_destroy( &key );
    }
    free( k ); free( iv ); free( aad ); free( pt ); free( ct );
    return ok;
}

static void selftest_asym_key( struct key *key, enum alg_id alg, enum ecc_curve_id curve, ULONG bitlen )
{
    memset( key, 0, sizeof(*key) );
    key->alg_id = alg;
    key->u.a.curve_id = curve;
    key->u.a.bitlen = bitlen;
    key->u.a.flags = KEY_FLAG_FINALIZED;
}

/* import an ECC blob made of the hex values (X, Y, D or X, Y) */
static BOOL selftest_import_ecc( struct key *key, DWORD magic, ULONG size, const char *xh, const char *yh,
                                 const char *dh )
{
    struct key_asymmetric_import_params p;
    ULONG xl, yl, dl = 0, len;
    UCHAR *x = hex( xh, &xl ), *y = hex( yh, &yl ), *d = dh ? hex( dh, &dl ) : NULL;
    BCRYPT_ECCKEY_BLOB *blob;
    BOOL ok = FALSE;

    len = sizeof(*blob) + size * (dh ? 3 : 2);
    if (x && y && (!dh || d) && (blob = calloc( 1, len )))
    {
        blob->dwMagic = magic;
        blob->cbKey = size;
        memcpy( (UCHAR *)(blob + 1), x, xl );
        memcpy( (UCHAR *)(blob + 1) + size, y, yl );
        if (d) memcpy( (UCHAR *)(blob + 1) + 2 * size, d, dl );
        p.key = key; p.flags = dh ? 0 : KEY_IMPORT_FLAG_PUBLIC; p.buf = (UCHAR *)blob; p.len = len;
        ok = !key_asymmetric_import( &p );
        free( blob );
    }
    free( x ); free( y ); free( d );
    return ok;
}

static BOOL selftest_derive( struct key *priv, struct key *pub, const char *want )
{
    struct key_asymmetric_derive_key_params p;
    UCHAR out[66];
    ULONG len = 0;

    p.privkey = priv; p.pubkey = pub; p.output = out; p.output_len = sizeof(out); p.ret_len = &len;
    return !key_asymmetric_derive_key( &p ) && hex_equal( out, len, want );
}

/* RFC 5903 section 8.1: ECDH on P-256 */
static BOOL selftest_ecdh_p256( void )
{
    struct key priv, pub;
    BOOL ok;

    selftest_asym_key( &priv, ALG_ID_ECDH_P256, ECC_CURVE_NONE, 256 );
    selftest_asym_key( &pub, ALG_ID_ECDH_P256, ECC_CURVE_NONE, 256 );
    ok = selftest_import_ecc( &priv, BCRYPT_ECDH_PRIVATE_P256_MAGIC, 32,
                              "dad0b65394221cf9b051e1feca5787d098dfe637fc90b9ef945d0c3772581180",
                              "5271a0461cdb8252d61f1c456fa3e59ab1f45b33accf5f58389e0577b8990bb3",
                              "c88f01f510d9ac3f70a292daa2316de544e9aab8afe84049c62a9c57862d1433" ) &&
         selftest_import_ecc( &pub, BCRYPT_ECDH_PUBLIC_P256_MAGIC, 32,
                              "d12dfb5289c8d4f81208b70270398c342296970a0bccb74c736fc7554494bf63",
                              "56fbf3ca366cc23e8157854c13c58d6aac23f046ada30f8353e74f33039872ab", NULL ) &&
         selftest_derive( &priv, &pub, "d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de" );
    key_asymmetric_destroy( &priv );
    key_asymmetric_destroy( &pub );
    return ok;
}

/* RFC 7748 section 5.2: X25519 */
static BOOL selftest_x25519( void )
{
    static const char zero32[] = "0000000000000000000000000000000000000000000000000000000000000000";
    struct key priv, pub;
    BOOL ok;

    selftest_asym_key( &priv, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    selftest_asym_key( &pub, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    ok = selftest_import_ecc( &priv, BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC, 32, zero32, zero32,
                              "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4" ) &&
         selftest_import_ecc( &pub, BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC, 32,
                              "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", zero32, NULL ) &&
         selftest_derive( &priv, &pub, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552" );
    key_asymmetric_destroy( &priv );
    key_asymmetric_destroy( &pub );
    return ok;
}

/* ECDSA P-256: a fresh key signs, the signature verifies, a changed digest doesn't */
static BOOL selftest_ecdsa( void )
{
    struct key key;
    struct key_asymmetric_sign_params s;
    struct key_asymmetric_verify_params v;
    UCHAR digest[32], sig[64];
    ULONG len = 0;
    BOOL ok;

    memset( digest, 0x5a, sizeof(digest) );
    selftest_asym_key( &key, ALG_ID_ECDSA_P256, ECC_CURVE_NONE, 256 );
    s.key = &key; s.padding = NULL; s.input = digest; s.input_len = 32; s.output = sig; s.output_len = 64;
    s.ret_len = &len; s.flags = 0;
    v.key = &key; v.padding = NULL; v.hash = digest; v.hash_len = 32; v.signature = sig; v.signature_len = 64; v.flags = 0;
    ok = !key_asymmetric_generate( &key ) && !key_asymmetric_sign( &s ) && len == 64 && !key_asymmetric_verify( &v );
    digest[0] ^= 1;
    ok = ok && key_asymmetric_verify( &v ) == STATUS_INVALID_SIGNATURE;
    key_asymmetric_destroy( &key );
    return ok;
}

/* RSA-2048 PKCS #1 v1.5 SHA-256 of "abc" from a fixed key imported as RSAPRIVATE (n, e, p, q), so
 * the completion of d, dp, dq, qinv is checked too. Key and expected signature made with
 *   openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out st.pem
 *   printf abc | openssl dgst -sha256 -sign st.pem */
static const char st_rsa_n[] =
    "ad0263bd04dd5a56c43c0455e6f5906c7e985d91ab0134cdbb5ed6a9838c4f2cfb7332a182bb3cd8933745b37bb5b7524d5b1535df87ae7de2"
    "c1d6a2d8226ac63cd5ee41083d912e6517d80e9ea5933f9f2868552bb7430f05ec987f6b9f033e5f48f5bf5764f17a553ce2f0fbbae75552fb"
    "1291099926f101ef288f4cb4a1f12f9ccf0b83f491bdb087fcbd323acae3c3f26f32a852a2871dba8a1f3459e28e5d9fba3595a77ced56f66e"
    "b9f09bfc41a72160ea90c5bcb573bb0b153743581c564111a72a90362c328db9052d3711fc98c6347bb93012b3f987bb0dd908e98af28eb864"
    "1ca4845b9f0166c16526d3f11b573a21f6ac8b4daf25dca1d158bbbb";
static const char st_rsa_p[] =
    "d978ed56f1477f12c67550c965d8b7facf99cb83cf9f4938a2b42554a37a91cdf1b465b1b8bf3dc03ee24cd5bd8a03e56256f9efc0f12f2b61"
    "b5df34593e29ba0551f1f13aa1ebb1aeb0ec0ef5704781397b114a8827cdf29f9ce694baefb20e60f663ab870eb5bd84d528eff8f82804308f"
    "16bc66b25405d9c1f31809bf79df";
static const char st_rsa_q[] =
    "cba8eba3fefd91bfc63b4982a337c289f4b65493903a181a946341d513f128feb86ea7a384160937d9624b0139c0409e9dcd7c162153cbe618"
    "900b46f4d540ca3f812ddc70f0a179f7b726bde7ff443c0fe4522b99487f148cccecd02e7ee23f0981fdb3ee3b8dbe8dd9d8290c14ff91d02f"
    "879dcec0ac4acb1b3fc3e7d4b1a5";
static const char st_rsa_sig[] =
    "1bed8a6b6532d594ecb5ebfc6d31b66d2472a3873c25a3dfb14f7adf85ef19b0b4d8211d3966e197a05eee45a8942fa08f73f2df45eecd0359"
    "93219d557e82348010687e95b3e995acac08c82c3a0cccbc202ad3be9952db06daff351eb4bde20d941f5c0c88156053ba71dfdce1cb8fa5e2"
    "6448840a988b92b8e035a61ce7f38c0986b31b568cf297583e01891402ccf48b58113015d737d284348f9fe175982bd180df98040644f043c5"
    "daea1df729200d6da0171f0ba8c6d2e5d18802ce55d42ed42077605f9090708300ff48a96dc897954b6de6d59fa862e5f6e7b24e7eb961b404"
    "eba0ebb84cec06e53f6d72b9d9a36e387395a0be41fb00dc8ddc794a";
/* SHA-256("abc"), FIPS 180-2 appendix B.1 */
static const char st_sha256_abc[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static BOOL selftest_rsa( void )
{
    struct key key;
    struct key_asymmetric_import_params ip;
    struct key_asymmetric_sign_params s;
    BCRYPT_PKCS1_PADDING_INFO pad = { BCRYPT_SHA256_ALGORITHM };
    BCRYPT_RSAKEY_BLOB *blob;
    ULONG nl, pl, ql, hl, len = 0, size;
    UCHAR *n = hex( st_rsa_n, &nl ), *p = hex( st_rsa_p, &pl ), *q = hex( st_rsa_q, &ql ), *h = hex( st_sha256_abc, &hl );
    UCHAR e[3] = { 0x01, 0x00, 0x01 }, sig[256];
    BOOL ok = FALSE;

    size = sizeof(*blob) + 3 + nl + pl + ql;
    if (n && p && q && h && (blob = calloc( 1, size )))
    {
        blob->Magic = BCRYPT_RSAPRIVATE_MAGIC;
        blob->BitLength = 2048;
        blob->cbPublicExp = 3; blob->cbModulus = nl; blob->cbPrime1 = pl; blob->cbPrime2 = ql;
        memcpy( (UCHAR *)(blob + 1), e, 3 );
        memcpy( (UCHAR *)(blob + 1) + 3, n, nl );
        memcpy( (UCHAR *)(blob + 1) + 3 + nl, p, pl );
        memcpy( (UCHAR *)(blob + 1) + 3 + nl + pl, q, ql );
        selftest_asym_key( &key, ALG_ID_RSA, ECC_CURVE_NONE, 2048 );
        ip.key = &key; ip.flags = 0; ip.buf = (UCHAR *)blob; ip.len = size;
        s.key = &key; s.padding = &pad; s.input = h; s.input_len = hl; s.output = sig; s.output_len = sizeof(sig);
        s.ret_len = &len; s.flags = BCRYPT_PAD_PKCS1;
        ok = !key_asymmetric_import( &ip ) && !key_asymmetric_sign( &s ) && hex_equal( sig, len, st_rsa_sig );
        key_asymmetric_destroy( &key );
        free( blob );
    }
    free( n ); free( p ); free( q ); free( h );
    return ok;
}

/* returns the number passed; the line names each failure */
static void run_selftest( void )
{
    static const struct { const char *name; BOOL (*test)( void ); } tests[] =
    {
        { "AES-128-CBC", NULL }, { "AES-128-CFB8", NULL }, { "AES-128-GCM", selftest_gcm },
        { "ECDH-P256", selftest_ecdh_p256 }, { "X25519", selftest_x25519 }, { "ECDSA-P256", selftest_ecdsa },
        { "RSA-2048-PKCS1-SHA256", selftest_rsa },
    };
    char failed[256] = "";
    unsigned int i, passed = 0;
    BOOL ok;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        if (i == 0)
            ok = selftest_aes( CHAIN_MODE_CBC, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51",
                               "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2" );
        else if (i == 1)
            ok = selftest_aes( CHAIN_MODE_CFB, "6bc1bee22e409f96e93d7e117393172aae2d",
                               "3b79424c9c0dd436bace9e0ed4586a4f32b9" );
        else ok = tests[i].test();
        if (ok) passed++;
        else
        {
            strncat( failed, " ", sizeof(failed) - strlen( failed ) - 1 );
            strncat( failed, tests[i].name, sizeof(failed) - strlen( failed ) - 1 );
        }
    }
    fprintf( stderr, "[bcrypt-ios] bcrypt self-test: %u/%u passed%s%s\n", passed, (unsigned)ARRAY_SIZE(tests),
             *failed ? ", failed:" : "", failed );
}

/* app-wide init state; not pthread_once (spec 09's no-exit rule) */
static int attach_state;    /* 0 not done, 1 in progress, 2 done */

static NTSTATUS bcrypt_process_attach( void *args )
{
    int expected = 0;

    if (__atomic_compare_exchange_n( &attach_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
    {
        if (ios_ltc_init()) fprintf( stderr, "[bcrypt-ios] LibTomCrypt set-up failed: DH and DSA will fail\n" );
        fprintf( stderr, "[bcrypt-ios] bcrypt: Security+CommonCrypto, BearSSL 7bea48e, LibTomCrypt 1.18.2\n" );
        run_selftest();
        __atomic_store_n( &attach_state, 2, __ATOMIC_RELEASE );
    }
    while (__atomic_load_n( &attach_state, __ATOMIC_ACQUIRE ) == 1) sched_yield();
    return STATUS_SUCCESS;
}

/* other programs keep using what the first attach set up: nothing is released */
static NTSTATUS bcrypt_process_detach( void *args )
{
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    bcrypt_process_attach,
    bcrypt_process_detach,
    key_symmetric_vector_reset,
    key_symmetric_set_auth_data,
    key_symmetric_encrypt,
    key_symmetric_decrypt,
    key_symmetric_get_tag,
    key_symmetric_destroy,
    key_asymmetric_generate,
    key_asymmetric_decrypt,
    key_asymmetric_encrypt,
    key_asymmetric_duplicate,
    key_asymmetric_sign,
    key_asymmetric_verify,
    key_asymmetric_destroy,
    key_asymmetric_export,
    key_asymmetric_import,
    key_asymmetric_derive_key,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

/* MYIOSDECK (spec 13 Part B): every PTR32 below is a guest address of the calling 32-bit
 * program; ios_g2h() turns it into a host pointer (B + value, 0 stays NULL). The key's private
 * data (a host pointer to our key state) is not an address of the program and is copied as is. */

typedef ULONG PTR32;

struct key_symmetric32
{
    enum chain_mode mode;
    ULONG           block_size;
    PTR32           vector;
    ULONG           vector_len;
    PTR32           secret;
    ULONG           secret_len;
    ULONG           __cs[6];
};

struct key_asymmetric32
{
    ULONG             bitlen;     /* ignored for ECC keys */
    enum ecc_curve_id curve_id;
    ULONG             flags;
    DSSSEED           dss_seed;
};

struct key32
{
    struct object hdr;
    enum alg_id   alg_id;
    UINT64        private[PRIVATE_DATA_SIZE];  /* private data for backend */
    union
    {
        struct key_symmetric32 s;
        struct key_asymmetric32 a;
    } u;
};

union padding
{
    BCRYPT_OAEP_PADDING_INFO oaep;
    BCRYPT_PKCS1_PADDING_INFO pkcs1;
    BCRYPT_PSS_PADDING_INFO pss;
};

union padding32
{
    struct
    {
        PTR32 pszAlgId;
        PTR32 pbLabel;
        ULONG cbLabel;
    } oaep;
    struct
    {
        PTR32 pszAlgId;
    } pkcs1;
    struct
    {
        PTR32 pszAlgId;
        ULONG cbSalt;
    } pss;
};

static union padding *get_padding( union padding32 *padding32, union padding *padding, ULONG flags)
{
    if (!padding32) return NULL;

    switch (flags)
    {
    case BCRYPT_PAD_OAEP:
        padding->oaep.pszAlgId = ios_g2h( padding32->oaep.pszAlgId );
        padding->oaep.pbLabel = ios_g2h( padding32->oaep.pbLabel );
        padding->oaep.cbLabel = padding32->oaep.cbLabel;
        return padding;

    case BCRYPT_PAD_PKCS1:
        padding->pkcs1.pszAlgId = ios_g2h( padding32->pkcs1.pszAlgId );
        return padding;

    case BCRYPT_PAD_PSS:
        padding->pss.pszAlgId = ios_g2h( padding32->pss.pszAlgId );
        padding->pss.cbSalt = padding32->pss.cbSalt;
        return padding;

    default:
        break;
    }
    return NULL;
}

static struct key *get_symmetric_key( struct key32 *key32, struct key *key )
{
    key->hdr            = key32->hdr;
    key->alg_id         = key32->alg_id;
    memcpy( key->private, key32->private, sizeof(key->private) );
    key->u.s.mode       = key32->u.s.mode;
    key->u.s.block_size = key32->u.s.block_size;
    key->u.s.vector     = ios_g2h(key32->u.s.vector);
    key->u.s.vector_len = key32->u.s.vector_len;
    key->u.s.secret     = ios_g2h(key32->u.s.secret);
    key->u.s.secret_len = key32->u.s.secret_len;
    return key;
}

static struct key *get_asymmetric_key( struct key32 *key32, struct key *key )
{
    key->hdr            = key32->hdr;
    key->alg_id         = key32->alg_id;
    memcpy( key->private, key32->private, sizeof(key->private) );
    key->u.a.bitlen     = key32->u.a.bitlen;
    key->u.a.curve_id   = key32->u.a.curve_id;
    key->u.a.flags      = key32->u.a.flags;
    key->u.a.dss_seed   = key32->u.a.dss_seed;
    return key;
}

static void put_symmetric_key32( struct key *key, struct key32 *key32 )
{
    memcpy( key32->private, key->private, sizeof(key32->private) );
}

static void put_asymmetric_key32( struct key *key, struct key32 *key32 )
{
    memcpy( key32->private, key->private, sizeof(key32->private) );
    key32->u.a.curve_id   = key->u.a.curve_id;
    key32->u.a.flags      = key->u.a.flags;
    key32->u.a.dss_seed   = key->u.a.dss_seed;
}

static NTSTATUS wow64_key_symmetric_vector_reset( void *args )
{
    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = args;

    ret = key_symmetric_vector_reset( get_symmetric_key( key32, &key ));
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_set_auth_data( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 auth_data;
        ULONG len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_symmetric_set_auth_data_params params =
    {
        get_symmetric_key( key32, &key ),
        ios_g2h(params32->auth_data),
        params32->len
    };

    ret = key_symmetric_set_auth_data( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_encrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_symmetric_encrypt_params params =
    {
        get_symmetric_key( key32, &key ),
        ios_g2h(params32->input),
        params32->input_len,
        ios_g2h(params32->output),
        params32->output_len
    };

    ret = key_symmetric_encrypt( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_decrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_symmetric_decrypt_params params =
    {
        get_symmetric_key( key32, &key ),
        ios_g2h(params32->input),
        params32->input_len,
        ios_g2h(params32->output),
        params32->output_len
    };

    ret = key_symmetric_decrypt( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_get_tag( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 tag;
        ULONG len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_symmetric_get_tag_params params =
    {
        get_symmetric_key( key32, &key ),
        ios_g2h(params32->tag),
        params32->len
    };

    ret = key_symmetric_get_tag( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_destroy( void *args )
{
    struct key32 *key32 = args;
    struct key key;
    NTSTATUS ret;

    ret = key_symmetric_destroy( get_symmetric_key( key32, &key ));
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_generate( void *args )
{
    struct key32 *key32 = args;
    struct key key;
    NTSTATUS ret;

    ret = key_asymmetric_generate( get_asymmetric_key( key32, &key ));
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_decrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 padding;
        PTR32 output;
        ULONG output_len;
        PTR32 ret_len;
        ULONG flags;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    union padding padding;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_asymmetric_decrypt_params params =
    {
        get_asymmetric_key( key32, &key ),
        ios_g2h(params32->input),
        params32->input_len,
        get_padding( ios_g2h(params32->padding), &padding, params32->flags ),
        ios_g2h(params32->output),
        params32->output_len,
        ios_g2h(params32->ret_len),
        params32->flags
    };

    ret = key_asymmetric_decrypt( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_encrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 padding;
        PTR32 output;
        ULONG output_len;
        PTR32 ret_len;
        ULONG flags;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    union padding padding;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_asymmetric_encrypt_params params =
    {
        get_asymmetric_key( key32, &key ),
        ios_g2h(params32->input),
        params32->input_len,
        get_padding( ios_g2h(params32->padding), &padding, params32->flags ),
        ios_g2h(params32->output),
        params32->output_len,
        ios_g2h(params32->ret_len),
        params32->flags
    };

    ret = key_asymmetric_encrypt( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_duplicate( void *args )
{
    struct
    {
        PTR32 key_orig;
        PTR32 key_copy;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key_orig, key_copy;
    struct key32 *key_orig32 = ios_g2h( params32->key_orig );
    struct key32 *key_copy32 = ios_g2h( params32->key_copy );
    struct key_asymmetric_duplicate_params params =
    {
        get_asymmetric_key( key_orig32, &key_orig ),
        get_asymmetric_key( key_copy32, &key_copy )
    };

    ret = key_asymmetric_duplicate( &params );
    put_asymmetric_key32( &key_copy, key_copy32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_sign( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 padding;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
        PTR32 ret_len;
        ULONG flags;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    union padding padding;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_asymmetric_sign_params params =
    {
        get_asymmetric_key( key32, &key ),
        get_padding(ios_g2h( params32->padding ), &padding, params32->flags),
        ios_g2h(params32->input),
        params32->input_len,
        ios_g2h(params32->output),
        params32->output_len,
        ios_g2h(params32->ret_len),
        params32->flags
    };

    ret = key_asymmetric_sign( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_verify( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 padding;
        PTR32 hash;
        ULONG hash_len;
        PTR32 signature;
        ULONG signature_len;
        ULONG flags;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    union padding padding;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_asymmetric_verify_params params =
    {
        get_asymmetric_key( key32, &key ),
        get_padding(ios_g2h( params32->padding ), &padding, params32->flags),
        ios_g2h(params32->hash),
        params32->hash_len,
        ios_g2h(params32->signature),
        params32->signature_len,
        params32->flags
    };

    ret = key_asymmetric_verify( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_destroy( void *args )
{
    struct key32 *key32 = args;
    struct key key;
    NTSTATUS ret;

    ret = key_asymmetric_destroy( get_asymmetric_key( key32, &key ));
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_export( void *args )
{
    struct
    {
        PTR32 key;
        ULONG flags;
        PTR32 buf;
        ULONG len;
        PTR32 ret_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_asymmetric_export_params params =
    {
        get_asymmetric_key( key32, &key ),
        params32->flags,
        ios_g2h(params32->buf),
        params32->len,
        ios_g2h(params32->ret_len),
    };

    ret = key_asymmetric_export( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_import( void *args )
{
    struct
    {
        PTR32 key;
        ULONG flags;
        PTR32 buf;
        ULONG len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ios_g2h( params32->key );
    struct key_asymmetric_import_params params =
    {
        get_asymmetric_key( key32, &key ),
        params32->flags,
        ios_g2h(params32->buf),
        params32->len
    };

    ret = key_asymmetric_import( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_derive_key( void *args )
{
    struct
    {
        PTR32 privkey;
        PTR32 pubkey;
        PTR32 output;
        ULONG output_len;
        PTR32 ret_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key privkey, pubkey;
    struct key32 *privkey32 = ios_g2h( params32->privkey );
    struct key32 *pubkey32 = ios_g2h( params32->pubkey );
    struct key_asymmetric_derive_key_params params =
    {
        get_asymmetric_key( privkey32, &privkey ),
        get_asymmetric_key( pubkey32, &pubkey ),
        ios_g2h(params32->output),
        params32->output_len,
        ios_g2h(params32->ret_len),
    };

    ret = key_asymmetric_derive_key( &params );
    put_asymmetric_key32( &privkey, privkey32 );
    put_asymmetric_key32( &pubkey, pubkey32 );
    return ret;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    bcrypt_process_attach,
    bcrypt_process_detach,
    wow64_key_symmetric_vector_reset,
    wow64_key_symmetric_set_auth_data,
    wow64_key_symmetric_encrypt,
    wow64_key_symmetric_decrypt,
    wow64_key_symmetric_get_tag,
    wow64_key_symmetric_destroy,
    wow64_key_asymmetric_generate,
    wow64_key_asymmetric_decrypt,
    wow64_key_asymmetric_encrypt,
    wow64_key_asymmetric_duplicate,
    wow64_key_asymmetric_sign,
    wow64_key_asymmetric_verify,
    wow64_key_asymmetric_destroy,
    wow64_key_asymmetric_export,
    wow64_key_asymmetric_import,
    wow64_key_asymmetric_derive_key,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif  /* _WIN64 */
