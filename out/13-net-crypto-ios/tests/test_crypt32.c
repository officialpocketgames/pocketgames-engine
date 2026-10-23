// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 15, crypt32: crypt32/unixlib.c (included below, with
// common/ios_rootenum.c) through its unix-call tables on the fake of include/ios_applecrypto.h:
// the root filter of clarification 12 (the fake distrusts certificates holding a marker; counts,
// the half-kept fallback, MADEIRA_CA_FILTER=0, the log line) and the PFX import of clarification 11
// (PKCS #1 -> PRIVATEKEYBLOB from the fixture key of tests/fixtures/fixtures.h).

#include "crypt32/unixlib.c"
#include "common/ios_rootenum.c"

#include <errno.h>
#include "fake_applecrypto.h"
#include "fixtures/fixtures.h"
#include "ios_test.h"

/* ---- stubs for what ntdll's unix side provides ---- */
unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel ) { return 0; }
const char * __cdecl __wine_dbg_strdup( const char *str ) { return str; }
int __cdecl __wine_dbg_output( const char *str ) { return fputs( str, stderr ); }
int __cdecl __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                               const char *function ) { return 0; }
static unsigned long fake_base;
unsigned long ios_wow_base( void ) { return fake_base; }
ULONG ios_wow_guest_addr( const void *host ) { return (ULONG)((ULONG_PTR)host - fake_base); }

#define CALL(f, p) __wine_unix_call_funcs[unix_##f]( p )
#define CALL32(f, p) __wine_unix_call_wow64_funcs[unix_##f]( p )

static size_t unhex( const char *s, unsigned char *out, size_t cap )
{
    size_t n = strlen( s ) / 2, i;
    unsigned int v;

    for (i = 0; i < n && i < cap; i++) { sscanf( s + 2 * i, "%2x", &v ); out[i] = v; }
    return i;
}

/* ================================================================ roots */

static char dir[256], bundle[300];

/* a stand-in certificate: SEQUENCE of 14 bytes of text (the fake only looks at the tag and for
 * the distrust marker) */
static size_t fake_cert( unsigned int i, BOOL bad, unsigned char *der )
{
    der[0] = 0x30;
    der[1] = 14;
    snprintf( (char *)der + 2, 15, "%s-cert-%04u", bad ? "BAD!" : "good", i );
    return 16;
}

static void base64( const unsigned char *in, size_t len, char *out )
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;

    for (i = 0; i + 2 < len; i += 3)
    {
        *out++ = tab[in[i] >> 2];
        *out++ = tab[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *out++ = tab[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        *out++ = tab[in[i + 2] & 63];
    }
    if (len - i == 1)
    {
        *out++ = tab[in[i] >> 2];
        *out++ = tab[(in[i] & 3) << 4];
        *out++ = '=';
        *out++ = '=';
    }
    else if (len - i == 2)
    {
        *out++ = tab[in[i] >> 2];
        *out++ = tab[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *out++ = tab[(in[i + 1] & 15) << 2];
        *out++ = '=';
    }
    *out = 0;
}

/* a PEM bundle of count certificates, those whose bit is set in bad_mask distrusted */
static void write_bundle( unsigned int count, unsigned int bad_mask )
{
    FILE *f = fopen( bundle, "w" );
    unsigned char der[32];
    char b64[64];
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        base64( der, fake_cert( i, (bad_mask >> i) & 1, der ), b64 );
        fprintf( f, "-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n", b64 );
    }
    fclose( f );
}

/* forget the loaded roots, as a new app run would */
static void reset_roots( void )
{
    struct root_cert *cert, *next;

    LIST_FOR_EACH_ENTRY_SAFE( cert, next, &root_cert_list, struct root_cert, entry )
    {
        list_remove( &cert->entry );
        free( cert );
    }
    free( ios_roots_sizes );
    free( ios_roots_data );
    ios_roots_sizes = NULL;
    ios_roots_data = NULL;
    ios_roots_count = 0;
    ios_roots_loaded = FALSE;
    memset( &ios_roots_positions, 0, sizeof(ios_roots_positions) );
}

static char log_text[4096];

/* one pass of enum_root_certs, stderr captured into log_text */
static unsigned int count_roots( void )
{
    static unsigned char buf[65536];
    struct enum_root_certs_params p;
    DWORD needed;
    unsigned int n = 0;
    char path[300];
    int saved, fd;
    ssize_t len;

    snprintf( path, sizeof(path), "%s/log.txt", dir );
    fflush( stderr );
    saved = dup( 2 );
    fd = open( path, O_WRONLY | O_CREAT | O_TRUNC, 0600 );
    dup2( fd, 2 );
    close( fd );

    p.buffer = buf;
    p.size = sizeof(buf);
    p.needed = &needed;
    while (n < 1000 && CALL( enum_root_certs, &p ) == STATUS_SUCCESS) n++;

    fflush( stderr );
    dup2( saved, 2 );
    close( saved );
    fd = open( path, O_RDONLY );
    len = read( fd, log_text, sizeof(log_text) - 1 );
    log_text[len > 0 ? len : 0] = 0;
    close( fd );
    unlink( path );
    fputs( log_text, stderr );
    return n;
}

static void test_roots( void )
{
    unsigned char der[32], digest[32];
    char want[64];
    br_sha256_context sha;
    unsigned int i;

    fake_ac_set_distrust_marker( "BAD!", 4 );

    /* unset: no roots */
    unsetenv( "MADEIRA_CA_BUNDLE" );
    reset_roots();
    CHECK_EQ( count_roots(), 0 );
    CHECK( strstr( log_text, "MADEIRA_CA_BUNDLE is not set" ) != NULL );

    /* 10 certificates, 3 distrusted (1, 4, 9): 7 kept */
    setenv( "MADEIRA_CA_BUNDLE", bundle, 1 );
    unsetenv( "MADEIRA_CA_FILTER" );
    write_bundle( 10, (1 << 1) | (1 << 4) | (1 << 9) );
    reset_roots();
    fake_ac_reset_counts();
    CHECK_EQ( count_roots(), 7 );
    CHECK_EQ( fake_ac_route_calls( FAKE_AC_TRUST ), 10 );
    CHECK( strstr( log_text, "10 certificates in the bundle, kept 7 of 10 roots, dropped 3, first SHA-256:" ) != NULL );
    br_sha256_init( &sha );
    br_sha256_update( &sha, der, fake_cert( 1, TRUE, der ) );
    br_sha256_out( &sha, digest );
    for (i = 0; i < 8; i++) sprintf( want + 2 * i, "%02x", digest[i] );
    CHECK( strstr( log_text, want ) != NULL );
    /* a second enumeration pass sees the same 7, without loading again */
    fake_ac_reset_counts();
    CHECK_EQ( count_roots(), 7 );
    CHECK_EQ( fake_ac_route_calls( FAKE_AC_TRUST ), 0 );

    /* 6 of 10 distrusted: fewer than half kept, the whole bundle is used */
    write_bundle( 10, 0x3f );
    reset_roots();
    CHECK_EQ( count_roots(), 10 );
    CHECK( strstr( log_text, "the device trusts only 4: filter not applied, kept 10 of 10 roots" ) != NULL );

    /* exactly half kept: filtered */
    write_bundle( 10, 0x1f );
    reset_roots();
    CHECK_EQ( count_roots(), 5 );

    /* MADEIRA_CA_FILTER=0: no trust calls, all kept */
    setenv( "MADEIRA_CA_FILTER", "0", 1 );
    write_bundle( 10, (1 << 1) | (1 << 4) | (1 << 9) );
    reset_roots();
    fake_ac_reset_counts();
    CHECK_EQ( count_roots(), 10 );
    CHECK_EQ( fake_ac_route_calls( FAKE_AC_TRUST ), 0 );
    CHECK( strstr( log_text, "filter off (MADEIRA_CA_FILTER=0): kept 10 of 10 roots" ) != NULL );
    unsetenv( "MADEIRA_CA_FILTER" );

    /* a directory is refused */
    setenv( "MADEIRA_CA_BUNDLE", dir, 1 );
    reset_roots();
    CHECK_EQ( count_roots(), 0 );
    CHECK( strstr( log_text, "not a regular file" ) != NULL );

    reset_roots();
    fake_ac_set_distrust_marker( NULL, 0 );
}

/* ================================================================ PFX */

static unsigned char cert_a[2048], key_der[1024];
static const unsigned char cert_b[] = { 0x30, 0x03, 'b', 'b', 'b' };
static size_t cert_a_len, key_der_len;

static void set_pfx( int32_t status, const uint16_t *password, unsigned int identities, BOOL key )
{
    const unsigned char *certs[3] = { cert_a, cert_b, cert_a };     /* a duplicate: dropped */
    size_t lens[3] = { cert_a_len, sizeof(cert_b), cert_a_len };

    fake_ac_set_pkcs12( status, password, certs, lens, 3, identities, key ? key_der : NULL, key ? key_der_len : 0 );
}

static NTSTATUS open_pfx( const WCHAR *password, cert_store_data_t *data, unsigned int *keys )
{
    static BYTE pfx_bytes[] = { 0x30, 0x00 };
    CRYPT_DATA_BLOB pfx = { sizeof(pfx_bytes), pfx_bytes };
    struct open_cert_store_params p = { &pfx, password, data, keys };

    *data = 0;
    *keys = 99;
    return CALL( open_cert_store, &p );
}

static void close_pfx( cert_store_data_t data )
{
    struct close_cert_store_params p = { data };
    CHECK_EQ( CALL( close_cert_store, &p ), STATUS_SUCCESS );
}

/* the fixture's hex value as len little-endian bytes */
static BOOL le_equals( const BYTE *blob, const char *hex, ULONG len )
{
    unsigned char be[256];
    size_t n = unhex( hex, be, sizeof(be) ), i;

    for (i = 0; i < len; i++) if (blob[i] != (i < n ? be[n - 1 - i] : 0)) return FALSE;
    return TRUE;
}

static void test_pfx( void )
{
    static const WCHAR pw[] = { 'p', 0xe9, 'w', 0 }, wrong[] = { 'p', 'w', 0 };
    static const uint16_t pw16[] = { 'p', 0xe9, 'w', 0 }, empty16[] = { 0 };
    struct import_store_key_params kp;
    struct import_store_cert_params cp;
    cert_store_data_t data;
    unsigned int keys;
    DWORD size;
    BYTE buf[2048], *ptr;
    BLOBHEADER *hdr = (BLOBHEADER *)buf;
    RSAPUBKEY *rsa = (RSAPUBKEY *)(hdr + 1);

    cert_a_len = unhex( fx_srv_cert, cert_a, sizeof(cert_a) );
    key_der_len = unhex( fx_rsa1024_der, key_der, sizeof(key_der) );

    /* a non-ASCII UTF-16 password reaches Security as given */
    set_pfx( 0, pw16, 2, TRUE );
    CHECK_EQ( open_pfx( pw, &data, &keys ), STATUS_SUCCESS );
    CHECK_EQ( keys, 1 );
    CHECK( data != 0 );

    /* the key: size query, then upstream's PRIVATEKEYBLOB */
    size = 0;
    kp.data = data; kp.buf = NULL; kp.buf_size = &size;
    CHECK_EQ( CALL( import_store_key, &kp ), STATUS_BUFFER_TOO_SMALL );
    CHECK_EQ( size, sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + 1024 * 9 / 16 );
    memset( buf, 0xcc, sizeof(buf) );
    kp.buf = buf;
    CHECK_EQ( CALL( import_store_key, &kp ), STATUS_SUCCESS );
    CHECK( hdr->bType == PRIVATEKEYBLOB && hdr->bVersion == CUR_BLOB_VERSION && !hdr->reserved &&
           hdr->aiKeyAlg == CALG_RSA_KEYX );
    CHECK( rsa->magic == RSA_MAGIC_KEY && rsa->bitlen == 1024 && rsa->pubexp == 65537 );
    ptr = (BYTE *)(rsa + 1);
    CHECK( le_equals( ptr, fx_rsa1024_n, 128 ) );          ptr += 128;
    CHECK( le_equals( ptr, fx_rsa1024_p, 64 ) );           ptr += 64;
    CHECK( le_equals( ptr, fx_rsa1024_q, 64 ) );           ptr += 64;
    CHECK( le_equals( ptr, fx_rsa1024_dp, 64 ) );          ptr += 64;
    CHECK( le_equals( ptr, fx_rsa1024_dq, 64 ) );          ptr += 64;
    CHECK( le_equals( ptr, fx_rsa1024_qinv, 64 ) );        ptr += 64;
    CHECK( le_equals( ptr, fx_rsa1024_d, 128 ) );          ptr += 128;
    CHECK_EQ( ptr - buf, size );
    CHECK_EQ( buf[size], 0xcc );

    /* the certificates, deduplicated, in order */
    size = 0;
    cp.data = data; cp.index = 0; cp.buf = NULL; cp.buf_size = &size;
    CHECK_EQ( CALL( import_store_cert, &cp ), STATUS_BUFFER_TOO_SMALL );
    CHECK_EQ( size, cert_a_len );
    cp.buf = buf;
    CHECK_EQ( CALL( import_store_cert, &cp ), STATUS_SUCCESS );
    CHECK( !memcmp( buf, cert_a, cert_a_len ) );
    cp.index = 1;
    CHECK_EQ( CALL( import_store_cert, &cp ), STATUS_SUCCESS );
    CHECK( !memcmp( buf, cert_b, sizeof(cert_b) ) );
    cp.index = 2;
    CHECK_EQ( CALL( import_store_cert, &cp ), STATUS_NO_MORE_ENTRIES );
    close_pfx( data );
    CHECK_EQ( fake_ac_live(), 0 );

    /* wrong password, an OSStatus failure: invalid parameter */
    set_pfx( 0, pw16, 1, TRUE );
    CHECK_EQ( open_pfx( wrong, &data, &keys ), STATUS_INVALID_PARAMETER );
    set_pfx( -26275 /* errSecDecode */, NULL, 1, TRUE );
    CHECK_EQ( open_pfx( pw, &data, &keys ), STATUS_INVALID_PARAMETER );

    /* NULL password = empty; certificates only: no key */
    set_pfx( 0, empty16, 0, FALSE );
    CHECK_EQ( open_pfx( NULL, &data, &keys ), STATUS_SUCCESS );
    CHECK_EQ( keys, 0 );
    kp.data = data; kp.buf = buf; size = sizeof(buf);
    CHECK_EQ( CALL( import_store_key, &kp ), STATUS_NOT_FOUND );
    close_pfx( data );

    /* no certificate at all: upstream's failure */
    fake_ac_set_pkcs12( 0, NULL, NULL, NULL, 0, 0, NULL, 0 );
    CHECK_EQ( open_pfx( pw, &data, &keys ), STATUS_INVALID_PARAMETER );
    CHECK_EQ( fake_ac_live(), 0 );
}

/* the 32-bit open_cert_store with B != 0: the blob and its pbData are guest addresses */
static void test_wow64( void )
{
    static unsigned char arena[8192] __attribute__((aligned(16)));
    static const uint16_t pw16[] = { 'x', 0 };
    struct { ULONG pfx, password, data_ret, key_count_ret; } *p32 = (void *)arena;
    struct { DWORD cbData; ULONG pbData; } *blob32 = (void *)(arena + 64);
    WCHAR *pw = (WCHAR *)(arena + 128);
    cert_store_data_t *data = (void *)(arena + 256);
    unsigned int *keys = (void *)(arena + 272);
    unsigned char *pfx = arena + 512;
    struct close_cert_store_params cp;

    fake_base = (unsigned long)(ULONG_PTR)arena - 0x20000;
#define G(p) ((ULONG)((ULONG_PTR)(p) - fake_base))
    set_pfx( 0, pw16, 1, TRUE );
    pw[0] = 'x'; pw[1] = 0;
    pfx[0] = 0x30; pfx[1] = 0;
    blob32->cbData = 2; blob32->pbData = G( pfx );
    p32->pfx = G( blob32 ); p32->password = G( pw ); p32->data_ret = G( data ); p32->key_count_ret = G( keys );
    *keys = 7;
    CHECK_EQ( CALL32( open_cert_store, p32 ), STATUS_SUCCESS );
    CHECK_EQ( *keys, 1 );
    cp.data = *data;
    CHECK_EQ( CALL32( close_cert_store, &cp ), STATUS_SUCCESS );
#undef G
    fake_base = 0;
    CHECK_EQ( fake_ac_live(), 0 );
}

int main( void )
{
    const char *tmp = getenv( "TMPDIR" );

    snprintf( dir, sizeof(dir), "%s/crypt32-test.XXXXXX", tmp && tmp[0] ? tmp : "/tmp" );
    if (!mkdtemp( dir ))
    {
        printf( "FAIL mkdtemp: %s\n", strerror( errno ) );
        return 1;
    }
    snprintf( bundle, sizeof(bundle), "%s/bundle.pem", dir );

    CHECK_EQ( CALL( process_attach, NULL ), STATUS_SUCCESS );
    CHECK_EQ( CALL32( process_attach, NULL ), STATUS_SUCCESS );
    test_roots();
    test_pfx();
    test_wow64();

    unlink( bundle );
    rmdir( dir );
    return ios_test_done( "test_crypt32" );
}
