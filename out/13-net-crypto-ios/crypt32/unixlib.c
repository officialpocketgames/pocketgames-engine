/*
 * Copyright 2019 Hans Leidekker for CodeWeavers
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
 * Changed for MYIOSDECK (2026, spec 13 Parts B and F, clarifications 11-13): GnuTLS replaced by
 * Security.framework: PFX import through SecPKCS12Import, and the root certificates of
 * MADEIRA_CA_BUNDLE kept only where the device trusts them as anchors (SecTrust); the roots are
 * loaded once per app run and every enumeration sees all of them; the 32-bit thunks convert guest
 * addresses with ios_g2h(). See out/13-net-crypto-ios/CHANGES.md.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>

#include "bearssl/bearssl.h"

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wincrypt.h"
#include "crypt32_private.h"
#include "wine/debug.h"
#include "ios_guest.h"
#include "ios_rootenum.h"
#include "ios_applecrypto.h"
#include "ios_der.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

static void log_once( int *flag, const char *msg )
{
    if (!__atomic_exchange_n( flag, 1, __ATOMIC_RELAXED )) fprintf( stderr, "[crypt32-ios] %s\n", msg );
}

/* MYIOSDECK (spec 13 clarification 13): nothing app-wide to set up for Security; every attach
 * succeeds */
static int attach_logged;

static NTSTATUS process_attach( void *args )
{
    log_once( &attach_logged, "crypt32: Security (PKCS #12 import, anchor trust), BearSSL 7bea48e (SHA-256)" );
    return STATUS_SUCCESS;
}

static NTSTATUS process_detach( void *args )
{
    return STATUS_SUCCESS;
}

#define RSA_MAGIC_KEY  ('R' | ('S' << 8) | ('A' << 16) | ('2' << 24))
#define RSA_PUBEXP     65537

/* clarification 11: what one PFX import gave */
struct cert_store_data
{
    struct ios_ac_p12 *p12;
    unsigned int key_bitlen;
};

static struct cert_store_data *get_store_data( cert_store_data_t data )
{
    return (struct cert_store_data *)(ULONG_PTR)data;
}

/* a big-endian value as len little-endian bytes (zero-padded) at *dst; FALSE when it doesn't fit */
static BOOL put_le( BYTE **dst, const struct ios_be *value, ULONG len )
{
    struct ios_be v = ios_be_trim( value->data, value->len );
    ULONG i;

    if (v.len > len) return FALSE;
    for (i = 0; i < len; i++) (*dst)[i] = i < v.len ? v.data[v.len - 1 - i] : 0;
    *dst += len;
    return TRUE;
}

/* clarification 11.3: SecKeyCopyExternalRepresentation's PKCS #1 RSAPrivateKey -> upstream's
 * PRIVATEKEYBLOB layout and size rule */
static NTSTATUS import_store_key( void *args )
{
    struct import_store_key_params *params = args;
    struct cert_store_data *data = get_store_data( params->data );
    struct ios_rsa_parts k;
    struct ios_be e;
    unsigned char *der;
    size_t der_len, i;
    unsigned int bitlen;
    int is_private;
    BLOBHEADER *hdr;
    RSAPUBKEY *rsakey;
    BYTE *dst;
    DWORD size, pubexp = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (!data->p12->key) return STATUS_NOT_FOUND;
    bitlen = data->key_bitlen;

    size = sizeof(*hdr) + sizeof(*rsakey) + (bitlen * 9 / 16);
    if (!params->buf || *params->buf_size < size)
    {
        *params->buf_size = size;
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (ios_ac_key_export( data->p12->key, &der, &der_len ))
    {
        fprintf( stderr, "[crypt32-ios] the PFX's private key can't be exported\n" );
        return STATUS_INVALID_PARAMETER;
    }
    if (ios_der_rsa_parse( der, der_len, &k, &is_private ) || !is_private)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    hdr = params->buf;
    hdr->bType    = PRIVATEKEYBLOB;
    hdr->bVersion = CUR_BLOB_VERSION;
    hdr->reserved = 0;
    hdr->aiKeyAlg = CALG_RSA_KEYX;

    e = ios_be_trim( k.e.data, k.e.len );
    for (i = 0; i < e.len && e.len <= sizeof(pubexp); i++) pubexp = (pubexp << 8) | e.data[i];
    rsakey = (RSAPUBKEY *)(hdr + 1);
    rsakey->magic  = RSA_MAGIC_KEY;
    rsakey->bitlen = bitlen;
    rsakey->pubexp = pubexp ? pubexp : RSA_PUBEXP;

    dst = (BYTE *)(rsakey + 1);
    if (!put_le( &dst, &k.n, bitlen / 8 ) || !put_le( &dst, &k.p, bitlen / 16 ) || !put_le( &dst, &k.q, bitlen / 16 ) ||
        !put_le( &dst, &k.dp, bitlen / 16 ) || !put_le( &dst, &k.dq, bitlen / 16 ) ||
        !put_le( &dst, &k.qinv, bitlen / 16 ) || !put_le( &dst, &k.d, bitlen / 8 ))
        status = STATUS_INVALID_PARAMETER;

done:
    memset( der, 0, der_len );
    free( der );
    return status;
}

/* clarification 11.1-11.2: SecPKCS12Import with the caller's UTF-16 password; certificates of
 * every item's chain (deduplicated), the first identity's RSA key */
static NTSTATUS open_cert_store( void *args )
{
    static int identities_logged;
    struct open_cert_store_params *params = args;
    struct cert_store_data *store_data;
    struct ios_ac_p12 *p12;
    enum ios_ac_key_type type;
    unsigned int bits = 0;
    int32_t os_status = 0;

    if (ios_ac_pkcs12_import( params->pfx->pbData, params->pfx->cbData, (const uint16_t *)params->password,
                              &p12, &os_status ))
    {
        fprintf( stderr, "[crypt32-ios] PFX import failed, OSStatus %d\n", (int)os_status );
        return STATUS_INVALID_PARAMETER;
    }
    if (!p12->cert_count)
    {
        fprintf( stderr, "[crypt32-ios] PFX import gave no certificate\n" );
        ios_ac_pkcs12_free( p12 );
        return STATUS_INVALID_PARAMETER;
    }
    if (p12->key && (ios_ac_key_info( p12->key, &type, &bits ) || type != IOS_AC_KEY_RSA))
    {
        FIXME( "key algorithm not supported\n" );
        ios_ac_pkcs12_free( p12 );
        return STATUS_INVALID_PARAMETER;
    }
    if (p12->identities > 1 && !__atomic_exchange_n( &identities_logged, 1, __ATOMIC_RELAXED ))
        fprintf( stderr, "[crypt32-ios] PFX with %u identities: the first one's key is used\n", p12->identities );

    if (!(store_data = malloc( sizeof(*store_data) )))
    {
        ios_ac_pkcs12_free( p12 );
        return STATUS_NO_MEMORY;
    }
    store_data->p12 = p12;
    store_data->key_bitlen = p12->key ? bits : 0;

    *params->data_ret = (ULONG_PTR)store_data;
    *params->key_count_ret = p12->key ? 1 : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS import_store_cert( void *args )
{
    struct import_store_cert_params *params = args;
    struct cert_store_data *data = get_store_data( params->data );
    size_t size;

    if (params->index >= data->p12->cert_count) return STATUS_NO_MORE_ENTRIES;

    size = data->p12->cert_lens[params->index];
    if (!params->buf || *params->buf_size < size)
    {
        *params->buf_size = size;
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy( params->buf, data->p12->certs[params->index], size );
    return STATUS_SUCCESS;
}

static NTSTATUS close_cert_store( void *args )
{
    struct close_cert_store_params *params = args;
    struct cert_store_data *data = get_store_data( params->data );

    if (params->data)
    {
        ios_ac_pkcs12_free( data->p12 );
        free( data );
    }
    return STATUS_SUCCESS;
}

struct root_cert
{
    struct list entry;
    SIZE_T      size;
    BYTE        data[1];
};

static struct list root_cert_list = LIST_INIT(root_cert_list);

static BYTE *add_cert( SIZE_T size )
{
    struct root_cert *cert = malloc( offsetof( struct root_cert, data[size] ));

    if (!cert) return NULL;
    cert->size = size;
    list_add_tail( &root_cert_list, &cert->entry );
    return cert->data;
}

struct DynamicBuffer
{
    DWORD allocated;
    DWORD used;
    char *data;
};

static inline void reset_buffer(struct DynamicBuffer *buffer)
{
    buffer->used = 0;
    if (buffer->data) buffer->data[0] = 0;
}

static void add_line_to_buffer(struct DynamicBuffer *buffer, LPCSTR line)
{
    if (buffer->used + strlen(line) + 1 > buffer->allocated)
    {
        DWORD new_size = max( max( buffer->allocated * 2, 1024 ), buffer->used + strlen(line) + 1 );
        void *ptr = realloc( buffer->data, new_size );
        if (!ptr) return;
        buffer->data = ptr;
        buffer->allocated = new_size;
        if (!buffer->used) buffer->data[0] = 0;
    }
    strcpy( buffer->data + buffer->used, line );
    buffer->used += strlen(line);
}

#define BASE64_DECODE_PADDING    0x100
#define BASE64_DECODE_WHITESPACE 0x200
#define BASE64_DECODE_INVALID    0x300

static inline int decodeBase64Byte(char c)
{
    int ret = BASE64_DECODE_INVALID;

    if (c >= 'A' && c <= 'Z')
        ret = c - 'A';
    else if (c >= 'a' && c <= 'z')
        ret = c - 'a' + 26;
    else if (c >= '0' && c <= '9')
        ret = c - '0' + 52;
    else if (c == '+')
        ret = 62;
    else if (c == '/')
        ret = 63;
    else if (c == '=')
        ret = BASE64_DECODE_PADDING;
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        ret = BASE64_DECODE_WHITESPACE;
    return ret;
}

static BOOL base64_to_cert( const char *str )
{
    DWORD i, valid, out, hasPadding;
    BYTE block[4], *data;

    for (i = valid = out = hasPadding = 0; str[i]; i++)
    {
        int d = decodeBase64Byte( str[i] );
        if (d == BASE64_DECODE_INVALID) return FALSE;
        if (d == BASE64_DECODE_WHITESPACE) continue;

        /* When padding starts, data is not acceptable */
        if (hasPadding && d != BASE64_DECODE_PADDING) return FALSE;

        /* Padding after a full block (like "VVVV=") is ok and stops decoding */
        if (d == BASE64_DECODE_PADDING && (valid & 3) == 0) break;

        valid++;
        if (d == BASE64_DECODE_PADDING)
        {
            hasPadding = 1;
            /* When padding reaches a full block, stop decoding */
            if ((valid & 3) == 0) break;
            continue;
        }

        /* out is incremented in the 4-char block as follows: "1-23" */
        if ((valid & 3) != 2) out++;
    }
    /* Fail if the block has bad padding; omitting padding is fine */
    if ((valid & 3) != 0 && hasPadding) return FALSE;

    if (!(data = add_cert( out ))) return FALSE;
    for (i = valid = out = 0; str[i]; i++)
    {
        int d = decodeBase64Byte( str[i] );
        if (d == BASE64_DECODE_WHITESPACE) continue;
        if (d == BASE64_DECODE_PADDING) break;
        block[valid & 3] = d;
        valid += 1;
        switch (valid & 3)
        {
        case 1:
            data[out++] = (block[0] << 2);
            break;
        case 2:
            data[out-1] = (block[0] << 2) | (block[1] >> 4);
            break;
        case 3:
            data[out++] = (block[1] << 4) | (block[2] >> 2);
            break;
        case 0:
            data[out++] = (block[2] << 6) | (block[3] >> 0);
            break;
        }
    }
    return TRUE;
}

/* Reads the file fd, and imports any certificates in it into store. */
static void import_certs_from_file( int fd )
{
    FILE *fp = fdopen(fd, "r");
    char line[1024];
    BOOL in_cert = FALSE;
    struct DynamicBuffer saved_cert = { 0, 0, NULL };
    int num_certs = 0;

    if (!fp) return;
    TRACE("\n");
    while (fgets(line, sizeof(line), fp))
    {
        static const char header[] = "-----BEGIN CERTIFICATE-----";
        static const char trailer[] = "-----END CERTIFICATE-----";

        if (!strncmp(line, header, strlen(header)))
        {
            TRACE("begin new certificate\n");
            in_cert = TRUE;
            reset_buffer(&saved_cert);
        }
        else if (!strncmp(line, trailer, strlen(trailer)))
        {
            TRACE("end of certificate, adding cert\n");
            in_cert = FALSE;
            if (base64_to_cert( saved_cert.data )) num_certs++;
        }
        else if (in_cert) add_line_to_buffer(&saved_cert, line);
    }
    free( saved_cert.data );
    TRACE("Read %d certs\n", num_certs);
    fclose(fp);
}

static void import_certs_from_path(LPCSTR path, BOOL allow_dir);

static BOOL check_buffer_resize(char **ptr_buf, size_t *buf_size, size_t check_size)
{
    if (check_size > *buf_size)
    {
        void *ptr = realloc(*ptr_buf, check_size);

        if (!ptr) return FALSE;
        *buf_size = check_size;
        *ptr_buf = ptr;
    }
    return TRUE;
}

/* Opens path, which must be a directory, and imports certificates from every
 * file in the directory into store.
 * Returns TRUE if any certificates were successfully imported.
 */
static void import_certs_from_dir( LPCSTR path )
{
    DIR *dir;

    dir = opendir(path);
    if (dir)
    {
        size_t path_len = strlen(path), bufsize = 0;
        char *filebuf = NULL;

        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            {
                size_t name_len = strlen(entry->d_name);

                if (!check_buffer_resize(&filebuf, &bufsize, path_len + 1 + name_len + 1)) break;
                snprintf(filebuf, bufsize, "%s/%s", path, entry->d_name);
                import_certs_from_path(filebuf, FALSE);
            }
        }
        free(filebuf);
        closedir(dir);
    }
}

/* Opens path, which may be a file or a directory, and imports any certificates
 * it finds into store.
 * Returns TRUE if any certificates were successfully imported.
 */
static void import_certs_from_path(LPCSTR path, BOOL allow_dir)
{
    int fd;

    TRACE("(%s, %d)\n", debugstr_a(path), allow_dir);

    fd = open(path, O_RDONLY);
    if (fd != -1)
    {
        struct stat st;

        if (fstat(fd, &st) == 0)
        {
            if (S_ISREG(st.st_mode))
                import_certs_from_file(fd);
            else if (S_ISDIR(st.st_mode))
            {
                if (allow_dir)
                    import_certs_from_dir(path);
                else
                    WARN("%s is a directory and directories are disallowed\n",
                     debugstr_a(path));
            }
            else
                ERR("%s: invalid file type\n", path);
        }
        close(fd);
    }
}

static uint64_t ios_now_ms(void)
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* MYIOSDECK (spec 13 Part F1): the PEM file named by MADEIRA_CA_BUNDLE, a regular file of
 * concatenated certificates (a directory is refused); FALSE when nothing could be read */
static BOOL ios_load_ca_bundle( const char *path )
{
    struct stat st;

    if (stat( path, &st ))
    {
        fprintf( stderr, "[crypt32-ios] MADEIRA_CA_BUNDLE=%s: cannot open it, no root certificates\n", path );
        return FALSE;
    }
    if (!S_ISREG( st.st_mode ))
    {
        fprintf( stderr, "[crypt32-ios] MADEIRA_CA_BUNDLE=%s: not a regular file, refused: no root certificates\n",
                 path );
        return FALSE;
    }
    import_certs_from_path( path, FALSE );
    return TRUE;
}

/* MYIOSDECK (spec 13 clarification 12): keep a bundle certificate only if the device trusts it
 * as an anchor; if that keeps fewer than half, keep them all; MADEIRA_CA_FILTER=0 turns the
 * filter off. One log line. */
static void ios_filter_roots( const char *path, uint64_t start )
{
    const char *flag = getenv( "MADEIRA_CA_FILTER" );
    struct root_cert *cert, *next;
    unsigned int total = 0, kept = 0, dropped = 0, i = 0, j;
    unsigned char *keep, digest[32];
    char fingerprints[8 * 18 + 1] = "", *p = fingerprints;
    br_sha256_context sha;

    LIST_FOR_EACH_ENTRY( cert, &root_cert_list, struct root_cert, entry ) total++;

    if (flag && !strcmp( flag, "0" ))
    {
        fprintf( stderr, "[crypt32-ios] MADEIRA_CA_BUNDLE=%s: %u certificates in the bundle, filter off "
                 "(MADEIRA_CA_FILTER=0): kept %u of %u roots, %u ms\n", path, total, total, total,
                 (unsigned int)(ios_now_ms() - start) );
        return;
    }

    if (!(keep = calloc( total ? total : 1, 1 ))) return;
    LIST_FOR_EACH_ENTRY( cert, &root_cert_list, struct root_cert, entry )
    {
        keep[i] = ios_ac_cert_is_trusted_anchor( cert->data, cert->size ) == 1;
        kept += keep[i++];
    }

    if (kept * 2 < total)
    {
        fprintf( stderr, "[crypt32-ios] MADEIRA_CA_BUNDLE=%s: %u certificates in the bundle, the device trusts "
                 "only %u: filter not applied, kept %u of %u roots, %u ms\n", path, total, kept, total, total,
                 (unsigned int)(ios_now_ms() - start) );
        free( keep );
        return;
    }

    i = 0;
    LIST_FOR_EACH_ENTRY_SAFE( cert, next, &root_cert_list, struct root_cert, entry )
    {
        if (!keep[i++])
        {
            if (dropped < 8)
            {
                br_sha256_init( &sha );
                br_sha256_update( &sha, cert->data, cert->size );
                br_sha256_out( &sha, digest );
                *p++ = ' ';
                for (j = 0; j < 8; j++) p += sprintf( p, "%02x", digest[j] );
            }
            dropped++;
            list_remove( &cert->entry );
            free( cert );
        }
    }
    free( keep );
    fprintf( stderr, "[crypt32-ios] MADEIRA_CA_BUNDLE=%s: %u certificates in the bundle, kept %u of %u roots, "
             "dropped %u%s%s, %u ms\n", path, total, kept, total, dropped, dropped ? ", first SHA-256:" : "",
             fingerprints, (unsigned int)(ios_now_ms() - start) );
}

static void load_root_certs(void)
{
    const char *path = getenv( "MADEIRA_CA_BUNDLE" );
    uint64_t start = ios_now_ms();

    if (!path || !path[0])
    {
        fprintf( stderr, "[crypt32-ios] MADEIRA_CA_BUNDLE is not set: no root certificates\n" );
        return;
    }
    if (ios_load_ca_bundle( path )) ios_filter_roots( path, start );
}

/* MYIOSDECK (spec 13 Part F2): the list is loaded once per app run and kept; each calling thread
 * walks it with its own position (ios_rootenum.c), so every program's root sync sees every
 * root. One lock covers the load, every walk and every position update. */
static pthread_mutex_t ios_roots_lock = PTHREAD_MUTEX_INITIALIZER;
static BOOL ios_roots_loaded;
static size_t ios_roots_count;
static size_t *ios_roots_sizes;
static const unsigned char **ios_roots_data;
static struct ios_rootenum ios_roots_positions;

static void ios_roots_load(void)
{
    struct root_cert *cert;
    size_t n = 0;

    load_root_certs();
    LIST_FOR_EACH_ENTRY( cert, &root_cert_list, struct root_cert, entry ) n++;
    ios_roots_sizes = malloc( (n ? n : 1) * sizeof(*ios_roots_sizes) );
    ios_roots_data = malloc( (n ? n : 1) * sizeof(*ios_roots_data) );
    if (!ios_roots_sizes || !ios_roots_data) n = 0;
    else
    {
        n = 0;
        LIST_FOR_EACH_ENTRY( cert, &root_cert_list, struct root_cert, entry )
        {
            ios_roots_sizes[n] = cert->size;
            ios_roots_data[n] = cert->data;
            n++;
        }
    }
    ios_roots_count = n;
}

/* a thread id never reused during the app run (a pthread_t value can be, after its thread
 * exits, which would hand a new thread a half pass) */
static uint64_t ios_thread_id(void)
{
#ifdef __APPLE__
    uint64_t tid;
    if (!pthread_threadid_np( NULL, &tid )) return tid;
#endif
    return (uint64_t)(uintptr_t)pthread_self();
}

static NTSTATUS enum_root_certs( void *args )
{
    struct enum_root_certs_params *params = args;
    enum ios_rootenum_result res;
    size_t needed = 0;

    pthread_mutex_lock( &ios_roots_lock );
    if (!ios_roots_loaded)
    {
        ios_roots_load();
        ios_roots_loaded = TRUE;
    }
    res = ios_rootenum_next( &ios_roots_positions, ios_thread_id(), ios_now_ms(),
                             ios_roots_count, ios_roots_sizes, (const unsigned char *const *)ios_roots_data,
                             params->buffer, params->size, &needed );
    pthread_mutex_unlock( &ios_roots_lock );

    if (res == IOS_ROOTENUM_END) return STATUS_NO_MORE_ENTRIES;
    *params->needed = needed;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    process_attach,
    process_detach,
    open_cert_store,
    import_store_key,
    import_store_cert,
    close_cert_store,
    enum_root_certs,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

/* MYIOSDECK (spec 13 Part B): every PTR32 below, and the PFX blob's pbData, is a guest address
 * of the calling 32-bit program; ios_g2h() turns it into a host pointer (B + value, 0 stays
 * NULL). cert_store_data_t is a handle and passes unchanged. */

typedef ULONG PTR32;

typedef struct
{
    DWORD cbData;
    PTR32 pbData;
} CRYPT_DATA_BLOB32;

static NTSTATUS wow64_open_cert_store( void *args )
{
    struct
    {
        PTR32 pfx;
        PTR32 password;
        PTR32 data_ret;
        PTR32 key_count_ret;
    } const *params32 = args;

    const CRYPT_DATA_BLOB32 *pfx32 = ios_g2h( params32->pfx );
    CRYPT_DATA_BLOB pfx = { pfx32->cbData, ios_g2h( pfx32->pbData ) };
    struct open_cert_store_params params =
    {
        &pfx,
        ios_g2h( params32->password ),
        ios_g2h( params32->data_ret ),
        ios_g2h( params32->key_count_ret )
    };

    return open_cert_store( &params );
}

static NTSTATUS wow64_import_store_key( void *args )
{
    struct
    {
        cert_store_data_t data;
        PTR32 buf;
        PTR32 buf_size;
    } const *params32 = args;

    struct import_store_key_params params =
    {
        params32->data,
        ios_g2h( params32->buf ),
        ios_g2h( params32->buf_size )
    };

    return import_store_key( &params );
}

static NTSTATUS wow64_import_store_cert( void *args )
{
    struct
    {
        cert_store_data_t data;
        unsigned int index;
        PTR32 buf;
        PTR32 buf_size;
    } const *params32 = args;

    struct import_store_cert_params params =
    {
        params32->data,
        params32->index,
        ios_g2h( params32->buf ),
        ios_g2h( params32->buf_size )
    };

    return import_store_cert( &params );
}

static NTSTATUS wow64_enum_root_certs( void *args )
{
    struct
    {
        PTR32  buffer;
        DWORD  size;
        PTR32  needed;
    } const *params32 = args;

    struct enum_root_certs_params params =
    {
        ios_g2h( params32->buffer ),
        params32->size,
        ios_g2h( params32->needed )
    };

    return enum_root_certs( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    process_attach,
    process_detach,
    wow64_open_cert_store,
    wow64_import_store_key,
    wow64_import_store_cert,
    close_cert_store,
    wow64_enum_root_certs,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif  /* _WIN64 */
