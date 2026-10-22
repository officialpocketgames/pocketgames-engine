// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 15: bcrypt's unix side (bcrypt/unixlib_ios.c, included below so its
// static helpers can be checked too) through its unix-call tables, on the host: the Apple calls go
// to tests/fake_applecrypto.c (BearSSL inside), BearSSL and LibTomCrypt are host builds of the
// copies in refs/ (tests/host-deps.sh). Compiled by run-host-tests.sh against the Wine headers.
//
// Vectors: NIST SP 800-38A F.1.1, F.2.1, F.3.7; McGrew-Viega GCM test cases 1-4 and 6 (SP 800-38D);
// RFC 5903 sections 8.1 and 8.2; RFC 7748 sections 5.2 and 6.1; RFC 7296 appendix B group 2.
// Keys: tests/fixtures/fixtures.h (tests/fixtures/make-fixtures.sh).

#include "bcrypt/unixlib_ios.c"

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

/* hex string -> malloc'ed bytes (unixlib_ios.c's hex()) */
static UCHAR *H( const char *s, ULONG *len )
{
    ULONG dummy;
    return hex( s, len ? len : &dummy );
}

static BOOL eq_hex( const UCHAR *data, ULONG len, const char *s )
{
    return hex_equal( data, len, s );
}

/* the hex value right-aligned in size bytes at dst */
static void put( UCHAR *dst, ULONG size, const char *s )
{
    ULONG len;
    UCHAR *v = H( s, &len );

    memset( dst, 0, size );
    if (len <= size) memcpy( dst + size - len, v, len );
    free( v );
}

/* ================================================================ symmetric */

static void sym_key( struct key *k, enum alg_id alg, enum chain_mode mode, UCHAR *secret, ULONG slen,
                     UCHAR *iv, ULONG ivlen )
{
    memset( k, 0, sizeof(*k) );
    k->alg_id = alg;
    k->u.s.mode = mode;
    k->u.s.block_size = alg == ALG_ID_AES ? 16 : 8;
    k->u.s.secret = secret;
    k->u.s.secret_len = slen;
    k->u.s.vector = iv;
    k->u.s.vector_len = ivlen;
}

static NTSTATUS enc( struct key *k, const UCHAR *in, ULONG len, UCHAR *out )
{
    struct key_symmetric_encrypt_params p = { k, in, len, out, len };
    return CALL( key_symmetric_encrypt, &p );
}

static NTSTATUS dec( struct key *k, const UCHAR *in, ULONG len, UCHAR *out )
{
    struct key_symmetric_decrypt_params p = { k, in, len, out, len };
    return CALL( key_symmetric_decrypt, &p );
}

static NTSTATUS get_tag( struct key *k, UCHAR *tag, ULONG len )
{
    struct key_symmetric_get_tag_params p = { k, tag, len };
    return CALL( key_symmetric_get_tag, &p );
}

static NTSTATUS auth( struct key *k, UCHAR *data, ULONG len )
{
    struct key_symmetric_set_auth_data_params p = { k, data, len };
    return CALL( key_symmetric_set_auth_data, &p );
}

static const char aes_key[] = "2b7e151628aed2a6abf7158809cf4f3c";
static const char aes_iv[] = "000102030405060708090a0b0c0d0e0f";
static const char aes_pt[] = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                             "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";

static void test_aes_cbc_ecb_cfb8( void )
{
    ULONG kl, ivl, pl;
    UCHAR *k = H( aes_key, &kl ), *iv = H( aes_iv, &ivl ), *pt = H( aes_pt, &pl );
    UCHAR *ct = H( "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                   "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7", NULL );
    UCHAR out[64];
    struct key key;

    /* CBC F.2.1, the chain split over two calls */
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_CBC, k, kl, iv, ivl );
    CHECK_EQ( enc( &key, pt, 16, out ), STATUS_SUCCESS );
    CHECK_EQ( enc( &key, pt + 16, 48, out + 16 ), STATUS_SUCCESS );
    CHECK( !memcmp( out, ct, 64 ) );
    /* vector_reset starts again from the key's vector; a change of direction continues the chain */
    CHECK_EQ( CALL( key_symmetric_vector_reset, &key ), STATUS_SUCCESS );
    CHECK_EQ( enc( &key, pt, 32, out ), STATUS_SUCCESS );
    CHECK_EQ( dec( &key, ct + 32, 32, out + 32 ), STATUS_SUCCESS );
    CHECK( !memcmp( out, ct, 32 ) && !memcmp( out + 32, pt + 32, 32 ) );
    /* decrypt in place, split */
    memcpy( out, ct, 64 );
    CHECK_EQ( CALL( key_symmetric_vector_reset, &key ), STATUS_SUCCESS );
    CHECK_EQ( dec( &key, out, 16, out ), STATUS_SUCCESS );
    CHECK_EQ( dec( &key, out + 16, 48, out + 16 ), STATUS_SUCCESS );
    CHECK( !memcmp( out, pt, 64 ) );
    CHECK_EQ( CALL( key_symmetric_destroy, &key ), STATUS_SUCCESS );

    /* NULL vector: zero IV, as GnuTLS */
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_CBC, k, kl, NULL, 0 );
    CHECK_EQ( enc( &key, pt, 16, out ), STATUS_SUCCESS );
    CHECK( eq_hex( out, 16, "3ad77bb40d7a3660a89ecaf32466ef97" ) );    /* = ECB of block 1 */
    CALL( key_symmetric_destroy, &key );

    /* ECB F.1.1 */
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_ECB, k, kl, NULL, 0 );
    CHECK_EQ( enc( &key, pt, 32, out ), STATUS_SUCCESS );
    CHECK( eq_hex( out, 32, "3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf" ) );
    CHECK_EQ( dec( &key, out, 32, out ), STATUS_SUCCESS );
    CHECK( !memcmp( out, pt, 32 ) );
    CALL( key_symmetric_destroy, &key );

    /* CFB8 F.3.7, split over calls, in place */
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_CFB, k, kl, iv, ivl );
    CHECK_EQ( enc( &key, pt, 5, out ), STATUS_SUCCESS );
    CHECK_EQ( enc( &key, pt + 5, 13, out + 5 ), STATUS_SUCCESS );
    CHECK( eq_hex( out, 18, "3b79424c9c0dd436bace9e0ed4586a4f32b9" ) );
    CHECK_EQ( CALL( key_symmetric_vector_reset, &key ), STATUS_SUCCESS );
    CHECK_EQ( dec( &key, out, 1, out ), STATUS_SUCCESS );
    CHECK_EQ( dec( &key, out + 1, 17, out + 1 ), STATUS_SUCCESS );
    CHECK( !memcmp( out, pt, 18 ) );
    /* encrypt 9, then decrypt the rest of the vector's ciphertext: the state carried over */
    CHECK_EQ( CALL( key_symmetric_vector_reset, &key ), STATUS_SUCCESS );
    CHECK_EQ( enc( &key, pt, 9, out ), STATUS_SUCCESS );
    {
        UCHAR *c = H( "3b79424c9c0dd436bace9e0ed4586a4f32b9", NULL );
        CHECK_EQ( dec( &key, c + 9, 9, out + 9 ), STATUS_SUCCESS );
        CHECK( !memcmp( out + 9, pt + 9, 9 ) );
        free( c );
    }
    CALL( key_symmetric_destroy, &key );

    CHECK_EQ( fake_ac_live(), 0 );
    free( k ); free( iv ); free( pt ); free( ct );
}

/* one GCM vector: encrypt (input split at split), tag; decrypt, tag; 16- and 12-byte tags */
static void gcm_case( const char *kh, const char *ivh, const char *ah, const char *ph, const char *ch,
                      const char *th, ULONG split )
{
    ULONG kl, ivl, al = 0, pl, cl;
    UCHAR *k = H( kh, &kl ), *iv = H( ivh, &ivl ), *a = ah ? H( ah, &al ) : NULL, *p = H( ph, &pl ), *c = H( ch, &cl );
    UCHAR out[128], tag[16];
    struct key key;

    if (split > pl) split = pl;
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_GCM, k, kl, iv, ivl );
    CHECK_EQ( auth( &key, a, al ), STATUS_SUCCESS );
    if (split) CHECK_EQ( enc( &key, p, split, out ), STATUS_SUCCESS );
    if (pl > split) CHECK_EQ( enc( &key, p + split, pl - split, out + split ), STATUS_SUCCESS );
    CHECK( !memcmp( out, c, pl ) );
    memset( tag, 0, sizeof(tag) );
    CHECK_EQ( get_tag( &key, tag, 16 ), STATUS_SUCCESS );
    CHECK( eq_hex( tag, 16, th ) );
    memset( tag, 0, sizeof(tag) );
    CHECK_EQ( get_tag( &key, tag, 12 ), STATUS_SUCCESS );
    CHECK( !memcmp( tag, H( th, NULL ), 12 ) && !tag[12] );
    /* vector_reset: the same again */
    CHECK_EQ( CALL( key_symmetric_vector_reset, &key ), STATUS_SUCCESS );
    CHECK_EQ( auth( &key, a, al ), STATUS_SUCCESS );
    CHECK_EQ( enc( &key, p, pl, out ), STATUS_SUCCESS );
    CHECK( !memcmp( out, c, pl ) );
    CALL( key_symmetric_destroy, &key );

    /* decrypt: the tag computed over the ciphertext equals the vector's */
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_GCM, k, kl, iv, ivl );
    CHECK_EQ( auth( &key, a, al ), STATUS_SUCCESS );
    memcpy( out, c, pl );
    if (split) CHECK_EQ( dec( &key, out, split, out ), STATUS_SUCCESS );
    if (pl > split) CHECK_EQ( dec( &key, out + split, pl - split, out + split ), STATUS_SUCCESS );
    CHECK( !memcmp( out, p, pl ) );
    CHECK_EQ( get_tag( &key, tag, 16 ), STATUS_SUCCESS );
    CHECK( eq_hex( tag, 16, th ) );
    CALL( key_symmetric_destroy, &key );

    free( k ); free( iv ); free( a ); free( p ); free( c );
}

static void test_aes_gcm( void )
{
    static const char k4[] = "feffe9928665731c6d6a8f9467308308";
    static const char iv4[] = "cafebabefacedbaddecaf888";
    static const char a4[] = "feedfacedeadbeeffeedfacedeadbeefabaddad2";
    static const char p4[] = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525"
                             "b16aedf5aa0de657ba637b39";
    static const char zero16[] = "00000000000000000000000000000000";
    ULONG kl, ivl, al;
    UCHAR *k, *iv, *a, tag1[16], tag2[16], tag0[16];
    struct key key;

    /* test case 1: no input, no auth data (NULL) */
    gcm_case( zero16, "000000000000000000000000", NULL, "", "", "58e2fccefa7e3061367f1d57a4e7455a", 0 );
    /* test case 2 */
    gcm_case( zero16, "000000000000000000000000", NULL, zero16, "0388dace60b6a392f328c2b971b2fe78",
              "ab6e47d42cec13bdf53a67b21257bddf", 7 );
    /* test case 3 */
    gcm_case( k4, iv4, NULL,
              "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525"
              "b16aedf5aa0de657ba637b391aafd255",
              "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa05"
              "1ba30b396a0aac973d58e091473f5985",
              "4d5c2af327cd64a62cf35abd2ba6fab4", 32 );
    /* test case 4, split inside a block */
    gcm_case( k4, iv4, a4, p4,
              "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa05"
              "1ba30b396a0aac973d58e091",
              "5bc94fbc3221a5db94fae95ae7121a47", 21 );
    /* test case 6: a 60-byte IV */
    gcm_case( k4,
              "9313225df88406e555909c5aff5269aa6a7a9538534f7da1e4c303d2a318a728c3c0c95156809539fcf0e2429a6b5254"
              "16aedbf5a0de6a57a637b39b", a4, p4,
              "8ce24998625615b603a033aca13fb894be9112a5c3a211a8ba262a3cca7e2ca701e4a9a4fba43c90ccdcb281d48c7c6f"
              "d62875d2aca417034c34aee5",
              "619cc5aefffe0bfa462af43c1699d050", 60 );

    /* auth data only: the tag depends on it, and encrypt and decrypt sides agree */
    k = H( k4, &kl ); iv = H( iv4, &ivl ); a = H( a4, &al );
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_GCM, k, kl, iv, ivl );
    CHECK_EQ( auth( &key, a, al ), STATUS_SUCCESS );
    CHECK_EQ( get_tag( &key, tag1, 16 ), STATUS_SUCCESS );
    CALL( key_symmetric_destroy, &key );
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_GCM, k, kl, iv, ivl );
    CHECK_EQ( auth( &key, a, al ), STATUS_SUCCESS );
    CHECK_EQ( dec( &key, NULL, 0, NULL ), STATUS_SUCCESS );
    CHECK_EQ( get_tag( &key, tag2, 16 ), STATUS_SUCCESS );
    CALL( key_symmetric_destroy, &key );
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_GCM, k, kl, iv, ivl );
    CHECK_EQ( get_tag( &key, tag0, 16 ), STATUS_SUCCESS );
    CALL( key_symmetric_destroy, &key );
    CHECK( !memcmp( tag1, tag2, 16 ) );
    CHECK( memcmp( tag1, tag0, 16 ) );
    /* auth data after the first encrypt: GnuTLS refused it */
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_GCM, k, kl, iv, ivl );
    CHECK_EQ( enc( &key, a, 16, tag0 ), STATUS_SUCCESS );
    CHECK_EQ( auth( &key, a, al ), STATUS_INTERNAL_ERROR );
    CALL( key_symmetric_destroy, &key );
    free( k ); free( iv ); free( a );
    CHECK_EQ( fake_ac_live(), 0 );
}

static void test_3des_and_unsupported( void )
{
    ULONG kl, ivl;
    UCHAR *k = H( "0123456789abcdeffedcba987654321089abcdef01234567", &kl ), *iv = H( "0011223344556677", &ivl );
    UCHAR in[16] = "sixteen bytes!!", out[16], back[16], k192[24] = {0}, buf[16];
    struct key key;

    sym_key( &key, ALG_ID_3DES, CHAIN_MODE_CBC, k, kl, iv, ivl );
    CHECK_EQ( enc( &key, in, 16, out ), STATUS_SUCCESS );
    CHECK( memcmp( out, in, 16 ) );
    CHECK_EQ( CALL( key_symmetric_vector_reset, &key ), STATUS_SUCCESS );
    CHECK_EQ( dec( &key, out, 16, back ), STATUS_SUCCESS );
    CHECK( !memcmp( back, in, 16 ) );
    CALL( key_symmetric_destroy, &key );

    /* a 16-byte 3DES key: GnuTLS's init failed */
    sym_key( &key, ALG_ID_3DES, CHAIN_MODE_CBC, k, 16, iv, ivl );
    CHECK_EQ( enc( &key, in, 16, out ), STATUS_INTERNAL_ERROR );
    CALL( key_symmetric_destroy, &key );

    sym_key( &key, ALG_ID_3DES, CHAIN_MODE_ECB, k, kl, NULL, 0 );
    CHECK_EQ( enc( &key, in, 16, out ), STATUS_NOT_SUPPORTED );
    CALL( key_symmetric_destroy, &key );
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_CCM, k192, 16, iv, 8 );
    CHECK_EQ( enc( &key, in, 16, out ), STATUS_NOT_SUPPORTED );
    CALL( key_symmetric_destroy, &key );
    sym_key( &key, ALG_ID_AES, CHAIN_MODE_GCM, k192, 24, buf, 12 );
    CHECK_EQ( enc( &key, in, 16, out ), STATUS_NOT_SUPPORTED );
    CALL( key_symmetric_destroy, &key );
    sym_key( &key, ALG_ID_RC4, CHAIN_MODE_CBC, k192, 16, NULL, 0 );
    CHECK_EQ( enc( &key, in, 16, out ), STATUS_NOT_SUPPORTED );
    CALL( key_symmetric_destroy, &key );

    free( k ); free( iv );
    CHECK_EQ( fake_ac_live(), 0 );
}

/* ================================================================ asymmetric helpers */

static void asym_key( struct key *k, enum alg_id alg, enum ecc_curve_id curve, ULONG bitlen )
{
    memset( k, 0, sizeof(*k) );
    k->alg_id = alg;
    k->u.a.curve_id = curve;
    k->u.a.bitlen = bitlen;
    k->u.a.flags = KEY_FLAG_FINALIZED;
}

static NTSTATUS import( struct key *k, ULONG flags, UCHAR *buf, ULONG len )
{
    struct key_asymmetric_import_params p = { k, flags, buf, len };
    return CALL( key_asymmetric_import, &p );
}

/* malloc'ed export, NULL on failure */
static UCHAR *export( struct key *k, ULONG flags, ULONG *len )
{
    struct key_asymmetric_export_params p = { k, flags, NULL, 0, len };
    NTSTATUS status;
    UCHAR *buf;

    *len = 0;
    status = CALL( key_asymmetric_export, &p );
    if (status && status != STATUS_BUFFER_TOO_SMALL) return NULL;
    if (!(buf = calloc( 1, *len ))) return NULL;
    p.buf = buf;
    p.len = *len;
    if (CALL( key_asymmetric_export, &p )) { free( buf ); return NULL; }
    return buf;
}

static NTSTATUS destroy( struct key *k )
{
    return CALL( key_asymmetric_destroy, k );
}

static NTSTATUS generate( struct key *k )
{
    return CALL( key_asymmetric_generate, k );
}

static NTSTATUS derive( struct key *priv, struct key *pub, UCHAR *out, ULONG out_len, ULONG *ret_len )
{
    struct key_asymmetric_derive_key_params p = { priv, pub, out, out_len, ret_len };
    return CALL( key_asymmetric_derive_key, &p );
}

static NTSTATUS sign( struct key *k, ULONG flags, void *pad, const UCHAR *in, ULONG len, UCHAR *out, ULONG out_len,
                      ULONG *ret_len )
{
    struct key_asymmetric_sign_params p = { k, pad, (UCHAR *)in, len, out, out_len, ret_len, flags };
    return CALL( key_asymmetric_sign, &p );
}

static NTSTATUS verify( struct key *k, ULONG flags, void *pad, const UCHAR *hash, ULONG len, const UCHAR *sig,
                        ULONG sig_len )
{
    struct key_asymmetric_verify_params p = { k, pad, (UCHAR *)hash, len, (UCHAR *)sig, sig_len, flags };
    return CALL( key_asymmetric_verify, &p );
}

static NTSTATUS rsa_enc( struct key *k, ULONG flags, void *pad, const UCHAR *in, ULONG len, UCHAR *out,
                         ULONG out_len, ULONG *ret_len )
{
    struct key_asymmetric_encrypt_params p = { k, (UCHAR *)in, len, pad, out, out_len, ret_len, flags };
    return CALL( key_asymmetric_encrypt, &p );
}

static NTSTATUS rsa_dec( struct key *k, ULONG flags, void *pad, const UCHAR *in, ULONG len, UCHAR *out,
                         ULONG out_len, ULONG *ret_len )
{
    struct key_asymmetric_decrypt_params p = { k, (UCHAR *)in, len, pad, out, out_len, ret_len, flags };
    return CALL( key_asymmetric_decrypt, &p );
}

/* ================================================================ RSA */

/* a BCRYPT RSA blob of the fixture key: RSAPRIVATE, or RSAFULLPRIVATE with dp dq qinv d */
static UCHAR *rsa1024_blob( ULONG magic, ULONG *size )
{
    BCRYPT_RSAKEY_BLOB *b;
    UCHAR *dst;
    BOOL full = magic == BCRYPT_RSAFULLPRIVATE_MAGIC;

    *size = sizeof(*b) + 3 + 128 + 64 + 64 + (full ? 64 * 3 + 128 : 0);
    b = calloc( 1, *size );
    b->Magic = magic;
    b->BitLength = 1024;
    b->cbPublicExp = 3; b->cbModulus = 128; b->cbPrime1 = 64; b->cbPrime2 = 64;
    dst = (UCHAR *)(b + 1);
    put( dst, 3, fx_rsa1024_e ); dst += 3;
    put( dst, 128, fx_rsa1024_n ); dst += 128;
    put( dst, 64, fx_rsa1024_p ); dst += 64;
    put( dst, 64, fx_rsa1024_q ); dst += 64;
    if (full)
    {
        put( dst, 64, fx_rsa1024_dp ); dst += 64;
        put( dst, 64, fx_rsa1024_dq ); dst += 64;
        put( dst, 64, fx_rsa1024_qinv ); dst += 64;
        put( dst, 128, fx_rsa1024_d );
    }
    return (UCHAR *)b;
}

static void import_rsa1024( struct key *k )
{
    ULONG size;
    UCHAR *blob = rsa1024_blob( BCRYPT_RSAPRIVATE_MAGIC, &size );

    asym_key( k, ALG_ID_RSA, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( import( k, 0, blob, size ), STATUS_SUCCESS );
    free( blob );
}

/* m = sig^e mod n with the fixture's public key (BearSSL), to see what a signature covers */
static void rsa1024_open( const UCHAR *sig, UCHAR *m )
{
    ULONG nl;
    UCHAR *n = H( fx_rsa1024_n, &nl ), e[3] = { 1, 0, 1 };
    br_rsa_public_key pk = { n, nl, e, 3 };

    memcpy( m, sig, 128 );
    CHECK( br_rsa_public_get_default()( m, 128, &pk ) );
    free( n );
}

static void test_rsa_import_export( void )
{
    struct key key, copy;
    ULONG size, len, len2;
    UCHAR *full = rsa1024_blob( BCRYPT_RSAFULLPRIVATE_MAGIC, &size ), *out, *out2;

    /* RSAPRIVATE: d, dp, dq, qinv completed with LibTomMath to the fixture's values */
    import_rsa1024( &key );
    out = export( &key, KEY_EXPORT_FLAG_RSA_FULL, &len );
    CHECK( out && len == size && !memcmp( out, full, size ) );
    free( out );
    /* RSAPRIVATE export */
    out = export( &key, 0, &len );
    CHECK( out && len == sizeof(BCRYPT_RSAKEY_BLOB) + 3 + 128 + 128 );
    CHECK( out && ((BCRYPT_RSAKEY_BLOB *)out)->Magic == BCRYPT_RSAPRIVATE_MAGIC );
    free( out );
    /* public */
    out = export( &key, KEY_EXPORT_FLAG_PUBLIC, &len );
    CHECK( out && len == sizeof(BCRYPT_RSAKEY_BLOB) + 3 + 128 &&
           !memcmp( out + sizeof(BCRYPT_RSAKEY_BLOB), full + sizeof(BCRYPT_RSAKEY_BLOB), 3 + 128 ) );
    CHECK( out && ((BCRYPT_RSAKEY_BLOB *)out)->Magic == BCRYPT_RSAPUBLIC_MAGIC &&
           ((BCRYPT_RSAKEY_BLOB *)out)->BitLength == 1024 && ((BCRYPT_RSAKEY_BLOB *)out)->cbPublicExp == 3 &&
           ((BCRYPT_RSAKEY_BLOB *)out)->cbModulus == 128 && !((BCRYPT_RSAKEY_BLOB *)out)->cbPrime1 );
    free( out );

    /* duplicate */
    asym_key( &copy, ALG_ID_RSA, ECC_CURVE_NONE, 1024 );
    {
        struct key_asymmetric_duplicate_params p = { &key, &copy };
        CHECK_EQ( CALL( key_asymmetric_duplicate, &p ), STATUS_SUCCESS );
    }
    out = export( &copy, KEY_EXPORT_FLAG_RSA_FULL, &len );
    CHECK( out && len == size && !memcmp( out, full, size ) );
    free( out );
    destroy( &copy );
    destroy( &key );

    /* RSAFULLPRIVATE round trip */
    asym_key( &key, ALG_ID_RSA, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( import( &key, 0, full, size ), STATUS_SUCCESS );
    out = export( &key, KEY_EXPORT_FLAG_RSA_FULL, &len );
    out2 = export( &key, KEY_EXPORT_FLAG_RSA_FULL, &len2 );
    CHECK( out && len == size && !memcmp( out, full, size ) && out2 && len2 == len );
    free( out ); free( out2 );
    /* not finalized: invalid handle */
    key.u.a.flags = 0;
    CHECK_EQ( export( &key, 0, &len ) == NULL, 1 );
    destroy( &key );
    free( full );
    CHECK_EQ( fake_ac_live(), 0 );
}

static void test_rsa_encrypt( void )
{
    static UCHAR label[] = "label";
    BCRYPT_OAEP_PADDING_INFO oaep = { BCRYPT_SHA256_ALGORITHM, NULL, 0 };
    BCRYPT_OAEP_PADDING_INFO oaep_label = { BCRYPT_SHA256_ALGORITHM, label, 5 };
    BCRYPT_OAEP_PADDING_INFO oaep_md5 = { BCRYPT_MD5_ALGORITHM, NULL, 0 };
    BCRYPT_OAEP_PADDING_INFO oaep_md2 = { BCRYPT_MD2_ALGORITHM, NULL, 0 };
    UCHAR msg[] = "message", ct[128], pt[128], n[128];
    ULONG len = 0, len2 = 0;
    struct key key;

    import_rsa1024( &key );

    /* size query */
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_PKCS1, NULL, msg, 7, NULL, 0, &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 128 );

    /* PKCS #1 v1.5 on SecKey */
    fake_ac_reset_counts();
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_PKCS1, NULL, msg, 7, ct, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_PKCS1, NULL, ct, 128, pt, 128, &len2 ), STATUS_SUCCESS );
    CHECK( len2 == 7 && !memcmp( pt, msg, 7 ) );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_ENC_PKCS1 ), 2 );
    /* flags 0: PKCS #1 too */
    CHECK_EQ( rsa_enc( &key, 0, NULL, msg, 7, ct, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_ENC_PKCS1 ), 3 );
    /* decrypt into a small buffer */
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_PKCS1, NULL, ct, 128, pt, 3, &len2 ), STATUS_BUFFER_TOO_SMALL );
    CHECK_EQ( len2, 7 );

    /* OAEP SHA-256 without a label: SecKey */
    fake_ac_reset_counts();
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_OAEP, &oaep, msg, 7, ct, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_OAEP, &oaep, ct, 128, pt, 128, &len2 ), STATUS_SUCCESS );
    CHECK( len2 == 7 && !memcmp( pt, msg, 7 ) );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_ENC_OAEP_SHA256 ), 2 );
    /* the label must match */
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_OAEP, &oaep_label, ct, 128, pt, 128, &len2 ), STATUS_INTERNAL_ERROR );

    /* OAEP with a label, and OAEP-MD5: BearSSL, no SecKey call */
    fake_ac_reset_counts();
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_OAEP, &oaep_label, msg, 7, ct, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_OAEP, &oaep_label, ct, 128, pt, 128, &len2 ), STATUS_SUCCESS );
    CHECK( len2 == 7 && !memcmp( pt, msg, 7 ) );
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_OAEP, &oaep_md5, msg, 7, ct + 0, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_OAEP, &oaep_md5, ct, 128, pt, 128, &len2 ), STATUS_SUCCESS );
    CHECK( len2 == 7 && !memcmp( pt, msg, 7 ) );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_ENC_OAEP_SHA256 ) + fake_ac_alg_calls( IOS_AC_RSA_ENC_OAEP_SHA1 ), 0 );
    /* OAEP-MD5 ciphertext through the SHA-256 route fails */
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_OAEP, &oaep, ct, 128, pt, 128, &len2 ), STATUS_INTERNAL_ERROR );
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_OAEP, &oaep_md2, msg, 7, ct, 128, &len ), STATUS_NOT_SUPPORTED );
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_OAEP, NULL, msg, 7, ct, 128, &len ), STATUS_INVALID_PARAMETER );

    /* PAD_NONE: raw, input zero-padded on the left; not below the modulus refused */
    fake_ac_reset_counts();
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_NONE, NULL, msg, 7, ct, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 128 );
    CHECK_EQ( rsa_dec( &key, BCRYPT_PAD_NONE, NULL, ct, 128, pt, 128, &len2 ), STATUS_SUCCESS );
    CHECK( len2 == 128 && !memcmp( pt + 121, msg, 7 ) && !pt[0] && !pt[120] );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_ENC_RAW ), 2 );
    put( n, 128, fx_rsa1024_n );
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_NONE, NULL, n, 128, ct, 128, &len ), STATUS_INVALID_PARAMETER );
    n[127]--;
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_NONE, NULL, n, 128, ct, 128, &len ), STATUS_SUCCESS );
    memset( n, 0xff, sizeof(n) );
    CHECK_EQ( rsa_enc( &key, BCRYPT_PAD_NONE, NULL, n, 129, ct, 128, &len ), STATUS_INVALID_PARAMETER );

    destroy( &key );
    CHECK_EQ( fake_ac_live(), 0 );
}

static void test_rsa_sign( void )
{
    BCRYPT_PKCS1_PADDING_INFO pkcs_sha256 = { BCRYPT_SHA256_ALGORITHM }, pkcs_md5 = { BCRYPT_MD5_ALGORITHM };
    BCRYPT_PKCS1_PADDING_INFO pkcs_null = { NULL }, pkcs_md4 = { BCRYPT_MD4_ALGORITHM };
    BCRYPT_PSS_PADDING_INFO pss32 = { BCRYPT_SHA256_ALGORITHM, 32 }, pss0 = { BCRYPT_SHA256_ALGORITHM, 0 };
    BCRYPT_PSS_PADDING_INFO pss20 = { BCRYPT_SHA256_ALGORITHM, 20 }, pss_md5 = { BCRYPT_MD5_ALGORITHM, 16 };
    BCRYPT_PSS_PADDING_INFO pss_md2 = { BCRYPT_MD2_ALGORITHM, 16 };
    UCHAR hash[64], sig[128], m[128];
    ULONG len = 0;
    struct key key;
    unsigned int i;

    for (i = 0; i < sizeof(hash); i++) hash[i] = i * 7 + 1;
    import_rsa1024( &key );

    /* output NULL: the size */
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, NULL, 0, &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 128 );
    CHECK_EQ( sign( &key, 0, NULL, hash, 32, sig, 128, &len ), STATUS_INVALID_PARAMETER );
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, NULL, hash, 32, sig, 128, &len ), STATUS_INVALID_PARAMETER );
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_md4, hash, 16, sig, 128, &len ), STATUS_NOT_SUPPORTED );

    /* PKCS #1 SHA-256 on SecKey */
    fake_ac_reset_counts();
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 128 );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 128 ), STATUS_SUCCESS );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_SIG_PKCS1_SHA256 ), 2 );
    sig[5] ^= 1;
    CHECK_EQ( verify( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 128 ), STATUS_INVALID_SIGNATURE );
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 64, &len ), STATUS_BUFFER_TOO_SMALL );

    /* MD5: PKCS1v15Raw over DigestInfo */
    fake_ac_reset_counts();
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_md5, hash, 16, sig, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_SIG_PKCS1_RAW ), 1 );
    rsa1024_open( sig, m );
    CHECK( m[0] == 0 && m[1] == 1 && m[2] == 0xff && !memcmp( m + 128 - 34, digestinfo_md5, 18 ) &&
           !memcmp( m + 128 - 16, hash, 16 ) && !m[128 - 35] );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PKCS1, &pkcs_md5, hash, 16, sig, 128 ), STATUS_SUCCESS );
    hash[0] ^= 1;
    CHECK_EQ( verify( &key, BCRYPT_PAD_PKCS1, &pkcs_md5, hash, 16, sig, 128 ), STATUS_INVALID_SIGNATURE );
    hash[0] ^= 1;

    /* NULL algorithm id: the input as given (36 bytes: TLS 1.0 MD5 || SHA-1) */
    fake_ac_reset_counts();
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_null, hash, 36, sig, 128, &len ), STATUS_SUCCESS );
    rsa1024_open( sig, m );
    CHECK( m[0] == 0 && m[1] == 1 && !m[128 - 37] && !memcmp( m + 128 - 36, hash, 36 ) );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PKCS1, &pkcs_null, hash, 36, sig, 128 ), STATUS_SUCCESS );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_SIG_PKCS1_RAW ), 2 );

    /* PSS, salt = hash length: SecKey */
    fake_ac_reset_counts();
    CHECK_EQ( sign( &key, BCRYPT_PAD_PSS, &pss32, hash, 32, sig, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PSS, &pss32, hash, 32, sig, 128 ), STATUS_SUCCESS );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_SIG_PSS_SHA256 ), 2 );
    /* PSS salt 0 and 20, and MD5: BearSSL */
    fake_ac_reset_counts();
    CHECK_EQ( sign( &key, BCRYPT_PAD_PSS, &pss0, hash, 32, sig, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PSS, &pss0, hash, 32, sig, 128 ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PSS, &pss32, hash, 32, sig, 128 ), STATUS_INVALID_SIGNATURE );
    CHECK_EQ( sign( &key, BCRYPT_PAD_PSS, &pss20, hash, 32, sig, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PSS, &pss20, hash, 32, sig, 128 ), STATUS_SUCCESS );
    sig[100] ^= 0x10;
    CHECK_EQ( verify( &key, BCRYPT_PAD_PSS, &pss20, hash, 32, sig, 128 ), STATUS_INVALID_SIGNATURE );
    CHECK_EQ( sign( &key, BCRYPT_PAD_PSS, &pss_md5, hash, 16, sig, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PSS, &pss_md5, hash, 16, sig, 128 ), STATUS_SUCCESS );
    CHECK_EQ( fake_ac_alg_calls( IOS_AC_RSA_SIG_PSS_SHA256 ), 1 );   /* the pss32 verify above */
    CHECK_EQ( sign( &key, BCRYPT_PAD_PSS, &pss_md2, hash, 16, sig, 128, &len ), STATUS_NOT_SUPPORTED );

    destroy( &key );
    CHECK_EQ( fake_ac_live(), 0 );
}

static void test_rsa_generate( void )
{
    BCRYPT_PKCS1_PADDING_INFO pkcs_sha256 = { BCRYPT_SHA256_ALGORITHM };
    UCHAR hash[32] = { 1, 2, 3 }, sig[256];
    ULONG len = 0;
    struct key key;
    UCHAR *pub;

    /* Security generates */
    fake_ac_reset_counts();
    asym_key( &key, ALG_ID_RSA, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( generate( &key ), STATUS_SUCCESS );
    CHECK_EQ( generate( &key ), STATUS_INVALID_HANDLE );
    CHECK_EQ( fake_ac_route_calls( FAKE_AC_KEY_GENERATE ), 1 );
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 128, &len ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 128 ), STATUS_SUCCESS );
    pub = export( &key, KEY_EXPORT_FLAG_PUBLIC, &len );
    CHECK( pub && len == sizeof(BCRYPT_RSAKEY_BLOB) + 3 + 128 );
    free( pub );
    destroy( &key );

    /* Security refuses 512 bits: BearSSL */
    fake_ac_reset_counts();
    asym_key( &key, ALG_ID_RSA_SIGN, ECC_CURVE_NONE, 512 );
    CHECK_EQ( generate( &key ), STATUS_SUCCESS );
    CHECK_EQ( fake_ac_route_calls( FAKE_AC_KEY_GENERATE_REFUSED ), 1 );
    CHECK_EQ( sign( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 64, &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 64 );
    CHECK_EQ( verify( &key, BCRYPT_PAD_PKCS1, &pkcs_sha256, hash, 32, sig, 64 ), STATUS_SUCCESS );
    destroy( &key );

    /* above BearSSL's limit */
    fake_ac_set_rsa_generate_range( 1024, 4096 );
    asym_key( &key, ALG_ID_RSA, ECC_CURVE_NONE, 8192 );
    CHECK_EQ( generate( &key ), STATUS_NOT_SUPPORTED );
    destroy( &key );
    CHECK_EQ( fake_ac_live(), 0 );
}

/* ================================================================ EC */

static void test_ecdsa( void )
{
    static const struct { enum alg_id alg; ULONG bits, coord, digest; } curves[] =
    {
        { ALG_ID_ECDSA_P256, 256, 32, 32 }, { ALG_ID_ECDSA_P384, 384, 48, 48 }, { ALG_ID_ECDSA_P521, 521, 66, 64 },
    };
    UCHAR hash[64], sig[132];
    ULONG len = 0, blen;
    struct key key, pub;
    unsigned int i, tries;
    UCHAR *blob;

    for (i = 0; i < sizeof(hash); i++) hash[i] = 0xa0 ^ i;
    for (i = 0; i < ARRAY_SIZE(curves); i++)
    {
        ULONG sig_len = curves[i].coord * 2, vlen = curves[i].digest == 64 ? 48 : curves[i].digest;

        asym_key( &key, curves[i].alg, ECC_CURVE_NONE, curves[i].bits );
        CHECK_EQ( generate( &key ), STATUS_SUCCESS );
        CHECK_EQ( sign( &key, 0, NULL, hash, vlen, sig, sizeof(sig), &len ), STATUS_SUCCESS );
        CHECK_EQ( len, sig_len );
        CHECK_EQ( verify( &key, 0, NULL, hash, vlen, sig, sig_len ), STATUS_SUCCESS );
        /* the public half alone */
        blob = export( &key, KEY_EXPORT_FLAG_PUBLIC, &blen );
        CHECK( blob && blen == sizeof(BCRYPT_ECCKEY_BLOB) + 2 * curves[i].coord );
        asym_key( &pub, curves[i].alg, ECC_CURVE_NONE, curves[i].bits );
        CHECK_EQ( import( &pub, KEY_IMPORT_FLAG_PUBLIC, blob, blen ), STATUS_SUCCESS );
        CHECK_EQ( verify( &pub, 0, NULL, hash, vlen, sig, sig_len ), STATUS_SUCCESS );
        hash[3] ^= 1;
        CHECK_EQ( verify( &pub, 0, NULL, hash, vlen, sig, sig_len ), STATUS_INVALID_SIGNATURE );
        hash[3] ^= 1;
        free( blob );
        /* private blob round trip */
        blob = export( &key, 0, &blen );
        CHECK( blob && blen == sizeof(BCRYPT_ECCKEY_BLOB) + 3 * curves[i].coord );
        destroy( &pub );
        asym_key( &pub, curves[i].alg, ECC_CURVE_NONE, curves[i].bits );
        CHECK_EQ( import( &pub, 0, blob, blen ), STATUS_SUCCESS );
        CHECK_EQ( sign( &pub, 0, NULL, hash, vlen, sig, sizeof(sig), &len ), STATUS_SUCCESS );
        CHECK_EQ( verify( &key, 0, NULL, hash, vlen, sig, sig_len ), STATUS_SUCCESS );
        free( blob );
        destroy( &pub );
        destroy( &key );
    }

    /* a 64-byte digest signed on P-256, a 20-byte one, sizes upstream refuses */
    asym_key( &key, ALG_ID_ECDSA_P256, ECC_CURVE_NONE, 256 );
    CHECK_EQ( generate( &key ), STATUS_SUCCESS );
    CHECK_EQ( sign( &key, 0, NULL, hash, 64, sig, sizeof(sig), &len ), STATUS_SUCCESS );
    CHECK_EQ( sign( &key, 0, NULL, hash, 20, sig, sizeof(sig), &len ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, 0, NULL, hash, 20, sig, 64 ), STATUS_SUCCESS );
    CHECK_EQ( sign( &key, 0, NULL, hash, 33, sig, sizeof(sig), &len ), STATUS_INVALID_PARAMETER );
    CHECK_EQ( verify( &key, 0, NULL, hash, 64, sig, 64 ), STATUS_INVALID_SIGNATURE );
    CHECK_EQ( sign( &key, 0, NULL, hash, 32, sig, 10, &len ), STATUS_BUFFER_TOO_SMALL );

    /* a signature whose s has a leading zero byte still verifies (minimal DER for Security) */
    for (tries = 0; tries < 5000; tries++)
    {
        hash[0] = tries; hash[1] = tries >> 8;
        CHECK_EQ( sign( &key, 0, NULL, hash, 32, sig, sizeof(sig), &len ), STATUS_SUCCESS );
        if (!sig[32] || !sig[0]) break;
    }
    CHECK( tries < 5000 );
    CHECK_EQ( verify( &key, 0, NULL, hash, 32, sig, 64 ), STATUS_SUCCESS );
    destroy( &key );
    CHECK_EQ( fake_ac_live(), 0 );
}

/* r || s <-> DER both ways: a high-bit r gets a 00 byte; a short s is minimal */
static void test_rs_der( void )
{
    UCHAR r[32], s[32], rs[64];
    struct buffer b;
    unsigned int i;

    for (i = 0; i < 32; i++) r[i] = 0x80 + i;
    memset( s, 0, sizeof(s) );
    s[31] = 5;
    buffer_init( &b );
    buffer_append_asn1_r_s( &b, r, 32, s + 31, 1 );
    CHECK( !b.error && b.pos == 2 + 2 + 33 + 3 );
    CHECK( b.buffer[0] == 0x30 && b.buffer[1] == 38 && b.buffer[2] == 2 && b.buffer[3] == 33 && b.buffer[4] == 0 &&
           !memcmp( b.buffer + 5, r, 32 ) && b.buffer[37] == 2 && b.buffer[38] == 1 && b.buffer[39] == 5 );
    CHECK_EQ( ios_der_sig_to_rs( b.buffer, b.pos, rs, 32 ), 0 );
    CHECK( !memcmp( rs, r, 32 ) && !memcmp( rs + 32, s, 32 ) );
    buffer_free( &b );
}

static void import_ecc( struct key *k, ULONG flags, DWORD magic, ULONG size, const char *x, const char *y,
                        const char *d )
{
    ULONG len = sizeof(BCRYPT_ECCKEY_BLOB) + size * (d ? 3 : 2);
    BCRYPT_ECCKEY_BLOB *b = calloc( 1, len );

    b->dwMagic = magic;
    b->cbKey = size;
    put( (UCHAR *)(b + 1), size, x );
    put( (UCHAR *)(b + 1) + size, size, y );
    if (d) put( (UCHAR *)(b + 1) + 2 * size, size, d );
    CHECK_EQ( import( k, flags, (UCHAR *)b, len ), STATUS_SUCCESS );
    free( b );
}

static void test_ecdh( void )
{
    static const char zero[] = "00";
    UCHAR out[66], out2[66], *blob, *blob2;
    ULONG len = 0, len2 = 0, blen, blen2;
    struct key a, b, pa, pb;

    /* RFC 5903 8.1 through generic ECDH on P-256 */
    asym_key( &a, ALG_ID_ECDH, ECC_CURVE_P256R1, 256 );
    asym_key( &pb, ALG_ID_ECDH, ECC_CURVE_P256R1, 256 );
    import_ecc( &a, 0, BCRYPT_ECDH_PRIVATE_P256_MAGIC, 32,
                "dad0b65394221cf9b051e1feca5787d098dfe637fc90b9ef945d0c3772581180",
                "5271a0461cdb8252d61f1c456fa3e59ab1f45b33accf5f58389e0577b8990bb3",
                "c88f01f510d9ac3f70a292daa2316de544e9aab8afe84049c62a9c57862d1433" );
    import_ecc( &pb, KEY_IMPORT_FLAG_PUBLIC, BCRYPT_ECDH_PUBLIC_P256_MAGIC, 32,
                "d12dfb5289c8d4f81208b70270398c342296970a0bccb74c736fc7554494bf63",
                "56fbf3ca366cc23e8157854c13c58d6aac23f046ada30f8353e74f33039872ab", NULL );
    fake_ac_reset_counts();
    CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
    CHECK( eq_hex( out, len, "d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de" ) );
    CHECK_EQ( fake_ac_route_calls( FAKE_AC_ECDH ), 1 );
    CHECK_EQ( derive( &a, &pb, NULL, 0, &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 32 );
    CHECK_EQ( derive( &a, &pb, out, 8, &len ), STATUS_BUFFER_TOO_SMALL );
    destroy( &a ); destroy( &pb );

    /* RFC 5903 8.2: P-384 */
    asym_key( &a, ALG_ID_ECDH_P384, ECC_CURVE_NONE, 384 );
    asym_key( &pb, ALG_ID_ECDH_P384, ECC_CURVE_NONE, 384 );
    import_ecc( &a, 0, BCRYPT_ECDH_PRIVATE_P384_MAGIC, 48,
                "667842d7d180ac2cde6f74f37551f55755c7645c20ef73e31634fe72b4c55ee6de3ac808acb4bdb4c88732aee95f41aa",
                "9482ed1fc0eeb9cafc4984625ccfc23f65032149e0e144ada024181535a0f38eeb9fcff3c2c947dae69b4c634573a81c",
                "099f3c7034d4a2c699884d73a375a67f7624ef7c6b3c0f160647b67414dce655e35b538041e649ee3faef896783ab194" );
    import_ecc( &pb, KEY_IMPORT_FLAG_PUBLIC, BCRYPT_ECDH_PUBLIC_P384_MAGIC, 48,
                "e558dbef53eecde3d3fccfc1aea08a89a987475d12fd950d83cfa41732bc509d0d1ac43a0336def96fda41d0774a3571",
                "dcfbec7aacf3196472169e838430367f66eebe3c6e70c416dd5f0c68759dd1fff83fa40142209dff5eaad96db9e6386c",
                NULL );
    CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
    CHECK( eq_hex( out, len, "11187331c279962d93d604243fd592cb9d0a926f422e47187521287e7156c5c4d603135569b9e9d0"
                             "9cf5d4a270f59746" ) );
    destroy( &a ); destroy( &pb );

    /* two generated P-521 keys agree */
    asym_key( &a, ALG_ID_ECDH_P521, ECC_CURVE_NONE, 521 );
    asym_key( &b, ALG_ID_ECDH_P521, ECC_CURVE_NONE, 521 );
    asym_key( &pa, ALG_ID_ECDH_P521, ECC_CURVE_NONE, 521 );
    asym_key( &pb, ALG_ID_ECDH_P521, ECC_CURVE_NONE, 521 );
    CHECK_EQ( generate( &a ), STATUS_SUCCESS );
    CHECK_EQ( generate( &b ), STATUS_SUCCESS );
    blob = export( &a, KEY_EXPORT_FLAG_PUBLIC, &blen );
    blob2 = export( &b, KEY_EXPORT_FLAG_PUBLIC, &blen2 );
    CHECK( blob && blob2 && blen == sizeof(BCRYPT_ECCKEY_BLOB) + 132 );
    CHECK_EQ( import( &pa, KEY_IMPORT_FLAG_PUBLIC, blob, blen ), STATUS_SUCCESS );
    CHECK_EQ( import( &pb, KEY_IMPORT_FLAG_PUBLIC, blob2, blen2 ), STATUS_SUCCESS );
    CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
    CHECK_EQ( derive( &b, &pa, out2, sizeof(out2), &len2 ), STATUS_SUCCESS );
    CHECK( len == 66 && len2 == 66 && !memcmp( out, out2, 66 ) );
    free( blob ); free( blob2 );
    destroy( &a ); destroy( &b ); destroy( &pa ); destroy( &pb );
    (void)zero;
    CHECK_EQ( fake_ac_live(), 0 );
}

static void test_x25519( void )
{
    static const char z[] = "00";
    static const char alice_d[] = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
    static const char alice_pub[] = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    static const char bob_d[] = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
    static const char bob_pub[] = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
    static const char shared[] = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
    UCHAR out[32], out2[32], *blob;
    ULONG len = 0, len2 = 0, blen;
    struct key a, b, pa, pb;

    /* 5.2 */
    asym_key( &a, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    asym_key( &pb, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    import_ecc( &a, 0, BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC, 32, z, z,
                "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4" );
    import_ecc( &pb, KEY_IMPORT_FLAG_PUBLIC, BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC, 32,
                "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", z, NULL );
    CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
    CHECK( eq_hex( out, len, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552" ) );
    destroy( &a ); destroy( &pb );

    /* 6.1: the public keys come out of the private imports; both sides agree */
    asym_key( &a, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    asym_key( &b, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    asym_key( &pa, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    asym_key( &pb, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    import_ecc( &a, 0, BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC, 32, z, z, alice_d );
    import_ecc( &b, 0, BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC, 32, z, z, bob_d );
    blob = export( &a, KEY_EXPORT_FLAG_PUBLIC, &blen );
    CHECK( blob && blen == sizeof(BCRYPT_ECCKEY_BLOB) + 64 && eq_hex( blob + sizeof(BCRYPT_ECCKEY_BLOB), 32, alice_pub ) );
    CHECK( blob && ((BCRYPT_ECCKEY_BLOB *)blob)->dwMagic == BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC );
    free( blob );
    import_ecc( &pa, KEY_IMPORT_FLAG_PUBLIC, BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC, 32, alice_pub, z, NULL );
    import_ecc( &pb, KEY_IMPORT_FLAG_PUBLIC, BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC, 32, bob_pub, z, NULL );
    CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
    CHECK_EQ( derive( &b, &pa, out2, sizeof(out2), &len2 ), STATUS_SUCCESS );
    CHECK( eq_hex( out, len, shared ) && eq_hex( out2, len2, shared ) );
    blob = export( &b, 0, &blen );
    CHECK( blob && blen == sizeof(BCRYPT_ECCKEY_BLOB) + 96 && eq_hex( blob + sizeof(BCRYPT_ECCKEY_BLOB), 32, bob_pub ) &&
           eq_hex( blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64, 32, bob_d ) );
    free( blob );
    destroy( &pa );

    /* an all-zero secret (u = 0) is refused */
    asym_key( &pa, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    import_ecc( &pa, KEY_IMPORT_FLAG_PUBLIC, BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC, 32, z, z, NULL );
    CHECK_EQ( derive( &b, &pa, out, sizeof(out), &len ), STATUS_INTERNAL_ERROR );
    destroy( &a ); destroy( &b ); destroy( &pa ); destroy( &pb );

    /* generated keys agree */
    asym_key( &a, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    asym_key( &b, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    asym_key( &pa, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    asym_key( &pb, ALG_ID_ECDH, ECC_CURVE_25519, 255 );
    CHECK_EQ( generate( &a ), STATUS_SUCCESS );
    CHECK_EQ( generate( &b ), STATUS_SUCCESS );
    blob = export( &a, KEY_EXPORT_FLAG_PUBLIC, &blen );
    CHECK_EQ( import( &pa, KEY_IMPORT_FLAG_PUBLIC, blob, blen ), STATUS_SUCCESS );
    free( blob );
    blob = export( &b, KEY_EXPORT_FLAG_PUBLIC, &blen );
    CHECK_EQ( import( &pb, KEY_IMPORT_FLAG_PUBLIC, blob, blen ), STATUS_SUCCESS );
    free( blob );
    CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
    CHECK_EQ( derive( &b, &pa, out2, sizeof(out2), &len2 ), STATUS_SUCCESS );
    CHECK( len == 32 && !memcmp( out, out2, 32 ) );
    destroy( &a ); destroy( &b ); destroy( &pa ); destroy( &pb );
    CHECK_EQ( fake_ac_live(), 0 );
}

/* ================================================================ DH */

static const char modp2[] =
    "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74020bbea63b139b22514a08798e3404dd"
    "ef9519b3cd3a431b302b0a6df25f14374fe1356d6d51c245e485b576625e7ec6f44c42e9a637ed6b0bff5cb6f406b7ed"
    "ee386bfb5a899fa5ae9f24117c4b1fe649286651ece65381ffffffffffffffff";

/* public half of k into a new key */
static void dh_public_of( struct key *k, struct key *pub )
{
    ULONG len;
    UCHAR *blob = export( k, KEY_EXPORT_FLAG_PUBLIC, &len );

    asym_key( pub, ALG_ID_DH, ECC_CURVE_NONE, k->u.a.bitlen );
    CHECK( blob != NULL );
    if (blob) CHECK_EQ( import( pub, KEY_IMPORT_FLAG_PUBLIC, blob, len ), STATUS_SUCCESS );
    free( blob );
}

static void test_dh( void )
{
    struct key a, b, c, pa, pb;
    UCHAR out[256], out2[256], *params, *params2, *priv, *priv2;
    ULONG len = 0, len2 = 0, plen, plen2, vlen, vlen2;
    BCRYPT_DH_PARAMETER_HEADER *hdr;
    unsigned int tries;
    BOOL found;

    /* without parameters, 1024 bits: RFC 7296 group 2 */
    asym_key( &a, ALG_ID_DH, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( generate( &a ), STATUS_SUCCESS );
    params = export( &a, KEY_EXPORT_FLAG_DH_PARAMETERS, &plen );
    CHECK( params && plen == sizeof(*hdr) + 256 );
    hdr = (BCRYPT_DH_PARAMETER_HEADER *)params;
    CHECK( params && hdr->dwMagic == BCRYPT_DH_PARAMETERS_MAGIC && hdr->cbKeyLength == 128 && hdr->cbLength == plen );
    CHECK( params && eq_hex( (UCHAR *)(hdr + 1), 128, modp2 ) );
    CHECK( params && ((UCHAR *)(hdr + 1))[255] == 2 );
    /* parameter blob round trip */
    asym_key( &c, ALG_ID_DH, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( import( &c, KEY_IMPORT_FLAG_DH_PARAMETERS, params, plen ), STATUS_SUCCESS );
    params2 = export( &c, KEY_EXPORT_FLAG_DH_PARAMETERS, &plen2 );
    CHECK( params2 && plen2 == plen && !memcmp( params, params2, plen ) );
    free( params ); free( params2 );
    destroy( &c );
    /* private blob round trip */
    priv = export( &a, 0, &vlen );
    CHECK( priv && vlen == sizeof(BCRYPT_DH_KEY_BLOB) + 4 * 128 );
    asym_key( &c, ALG_ID_DH, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( import( &c, 0, priv, vlen ), STATUS_SUCCESS );
    priv2 = export( &c, 0, &vlen2 );
    CHECK( priv2 && vlen2 == vlen && !memcmp( priv, priv2, vlen ) );
    free( priv ); free( priv2 );
    destroy( &c ); destroy( &a );

    /* two 2048-bit keys agree; the second takes the first's parameters */
    asym_key( &a, ALG_ID_DH, ECC_CURVE_NONE, 2048 );
    asym_key( &b, ALG_ID_DH, ECC_CURVE_NONE, 2048 );
    CHECK_EQ( generate( &a ), STATUS_SUCCESS );
    params = export( &a, KEY_EXPORT_FLAG_DH_PARAMETERS, &plen );
    CHECK_EQ( import( &b, KEY_IMPORT_FLAG_DH_PARAMETERS, params, plen ), STATUS_SUCCESS );
    free( params );
    CHECK_EQ( generate( &b ), STATUS_SUCCESS );
    dh_public_of( &a, &pa );
    dh_public_of( &b, &pb );
    CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
    CHECK_EQ( derive( &b, &pa, out2, sizeof(out2), &len2 ), STATUS_SUCCESS );
    CHECK( len == 256 && len2 == 256 && !memcmp( out, out2, 256 ) );
    destroy( &a ); destroy( &b ); destroy( &pa ); destroy( &pb );

    /* a secret with a leading zero byte comes back padded to the modulus length (768 bits: group 1) */
    asym_key( &a, ALG_ID_DH, ECC_CURVE_NONE, 768 );
    CHECK_EQ( generate( &a ), STATUS_SUCCESS );
    params = export( &a, KEY_EXPORT_FLAG_DH_PARAMETERS, &plen );
    found = FALSE;
    for (tries = 0; tries < 4000 && !found; tries++)
    {
        asym_key( &b, ALG_ID_DH, ECC_CURVE_NONE, 768 );
        CHECK_EQ( import( &b, KEY_IMPORT_FLAG_DH_PARAMETERS, params, plen ), STATUS_SUCCESS );
        CHECK_EQ( generate( &b ), STATUS_SUCCESS );
        dh_public_of( &b, &pb );
        CHECK_EQ( derive( &a, &pb, out, sizeof(out), &len ), STATUS_SUCCESS );
        CHECK_EQ( len, 96 );
        if (!out[0])
        {
            found = TRUE;
            dh_public_of( &a, &pa );
            CHECK_EQ( derive( &b, &pa, out2, sizeof(out2), &len2 ), STATUS_SUCCESS );
            CHECK( len2 == 96 && !memcmp( out, out2, 96 ) );
            destroy( &pa );
        }
        destroy( &b ); destroy( &pb );
    }
    CHECK( found );
    free( params );
    destroy( &a );
    CHECK_EQ( fake_ac_live(), 0 );
}

/* ================================================================ DSA */

static void test_dsa( void )
{
    UCHAR hash[20], sig[40], *blob, *blob2, *pub, *pub2;
    ULONG len = 0, blen, blen2, publen, publen2;
    struct key key, k2, k3;

    memset( hash, 0x3c, sizeof(hash) );
    asym_key( &key, ALG_ID_DSA, ECC_CURVE_NONE, 1000 );
    CHECK_EQ( generate( &key ), STATUS_NOT_SUPPORTED );
    destroy( &key );

    asym_key( &key, ALG_ID_DSA, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( generate( &key ), STATUS_SUCCESS );
    CHECK_EQ( sign( &key, 0, NULL, hash, 20, NULL, 0, &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 128 );   /* upstream: the key size when output is NULL */
    CHECK_EQ( sign( &key, 0, NULL, hash, 20, sig, sizeof(sig), &len ), STATUS_SUCCESS );
    CHECK_EQ( len, 40 );
    CHECK_EQ( sign( &key, 0, NULL, hash, 32, sig, sizeof(sig), &len ), STATUS_INVALID_PARAMETER );
    CHECK_EQ( verify( &key, 0, NULL, hash, 20, sig, 40 ), STATUS_SUCCESS );
    hash[7] ^= 1;
    CHECK_EQ( verify( &key, 0, NULL, hash, 20, sig, 40 ), STATUS_INVALID_SIGNATURE );
    hash[7] ^= 1;

    /* BCRYPT public blob round trip */
    pub = export( &key, KEY_EXPORT_FLAG_PUBLIC, &publen );
    CHECK( pub && publen == sizeof(BCRYPT_DSA_KEY_BLOB) + 3 * 128 );
    CHECK( pub && ((BCRYPT_DSA_KEY_BLOB *)pub)->dwMagic == BCRYPT_DSA_PUBLIC_MAGIC );
    asym_key( &k2, ALG_ID_DSA, ECC_CURVE_NONE, 1024 );
    CHECK_EQ( import( &k2, KEY_IMPORT_FLAG_PUBLIC, pub, publen ), STATUS_SUCCESS );
    CHECK_EQ( verify( &k2, 0, NULL, hash, 20, sig, 40 ), STATUS_SUCCESS );
    pub2 = export( &k2, KEY_EXPORT_FLAG_PUBLIC, &publen2 );
    CHECK( pub2 && publen2 == publen && !memcmp( pub, pub2, publen ) );
    free( pub ); free( pub2 );
    destroy( &k2 );
    /* a BCRYPT private export: not implemented upstream */
    CHECK( export( &key, 0, &blen ) == NULL );

    /* CAPI v2 with DSSSEED */
    key.u.a.flags |= KEY_FLAG_LEGACY_DSA_V2;
    key.u.a.dss_seed.counter = 0x1234;
    memset( key.u.a.dss_seed.seed, 0xab, sizeof(key.u.a.dss_seed.seed) );
    blob = export( &key, 0, &blen );
    CHECK( blob && blen == sizeof(BLOBHEADER) + sizeof(DSSPUBKEY) + 128 + 20 + 128 + 20 + sizeof(DSSSEED) );
    CHECK( blob && ((BLOBHEADER *)blob)->bType == PRIVATEKEYBLOB && ((DSSPUBKEY *)(blob + 8))->magic == MAGIC_DSS2 );
    pub = export( &key, KEY_EXPORT_FLAG_PUBLIC, &publen );
    CHECK( pub && publen == sizeof(BLOBHEADER) + sizeof(DSSPUBKEY) + 128 + 20 + 128 + 128 + sizeof(DSSSEED) );
    CHECK( pub && ((BLOBHEADER *)pub)->bType == PUBLICKEYBLOB && ((DSSPUBKEY *)(pub + 8))->magic == MAGIC_DSS1 );

    asym_key( &k2, ALG_ID_DSA, ECC_CURVE_NONE, 1024 );
    k2.u.a.flags |= KEY_FLAG_LEGACY_DSA_V2;
    CHECK_EQ( import( &k2, 0, blob, blen ), STATUS_SUCCESS );
    CHECK_EQ( k2.u.a.dss_seed.counter, 0x1234 );
    blob2 = export( &k2, 0, &blen2 );
    CHECK( blob2 && blen2 == blen && !memcmp( blob, blob2, blen ) );
    /* y recomputed from x after the private import */
    pub2 = export( &k2, KEY_EXPORT_FLAG_PUBLIC, &publen2 );
    CHECK( pub2 && publen2 == publen && !memcmp( pub, pub2, publen ) );
    CHECK_EQ( sign( &k2, 0, NULL, hash, 20, sig, sizeof(sig), &len ), STATUS_SUCCESS );
    free( blob2 ); free( pub2 );

    asym_key( &k3, ALG_ID_DSA, ECC_CURVE_NONE, 1024 );
    k3.u.a.flags |= KEY_FLAG_LEGACY_DSA_V2;
    CHECK_EQ( import( &k3, KEY_IMPORT_FLAG_PUBLIC, pub, publen ), STATUS_SUCCESS );
    CHECK_EQ( verify( &k3, 0, NULL, hash, 20, sig, 40 ), STATUS_SUCCESS );
    CHECK_EQ( verify( &key, 0, NULL, hash, 20, sig, 40 ), STATUS_SUCCESS );
    free( blob ); free( pub );
    destroy( &k3 ); destroy( &k2 ); destroy( &key );
    CHECK_EQ( fake_ac_live(), 0 );
}

/* ================================================================ 32-bit thunks, attach, self-test */

static void test_wow64( void )
{
    static UCHAR arena[16384] __attribute__((aligned(16)));
    struct
    {
        ULONG key, input, input_len, output, output_len;
    } *enc32;
    struct
    {
        ULONG key, padding, input, input_len, output, output_len, ret_len, flags;
    } *sign32;
    struct key32 *k32;
    struct key native;
    UCHAR *k, *iv, *pt, *out, *sig;
    WCHAR *alg;
    ULONG *pad32, *ret_len, kl, ivl, pl;
    BCRYPT_PKCS1_PADDING_INFO pkcs = { BCRYPT_SHA256_ALGORITHM };
    UCHAR hash[32] = { 9, 8, 7 };
    ULONG len = 0;

    /* guest address g lives at arena + g - 0x10000 */
    fake_base = (unsigned long)(ULONG_PTR)arena - 0x10000;
#define G(p) ((ULONG)((ULONG_PTR)(p) - fake_base))

    /* AES-CBC encrypt through the 32-bit table */
    k32 = (struct key32 *)arena;
    k = arena + 512; iv = arena + 576; pt = arena + 640; out = arena + 768;
    enc32 = (void *)(arena + 1024);
    memcpy( k, H( aes_key, &kl ), 16 );
    memcpy( iv, H( aes_iv, &ivl ), 16 );
    memcpy( pt, H( aes_pt, &pl ), 64 );
    memset( k32, 0, sizeof(*k32) );
    k32->alg_id = ALG_ID_AES;
    k32->u.s.mode = CHAIN_MODE_CBC;
    k32->u.s.block_size = 16;
    k32->u.s.vector = G( iv ); k32->u.s.vector_len = 16;
    k32->u.s.secret = G( k ); k32->u.s.secret_len = 16;
    enc32->key = G( k32 ); enc32->input = G( pt ); enc32->input_len = 32; enc32->output = G( out ); enc32->output_len = 32;
    CHECK_EQ( CALL32( key_symmetric_encrypt, enc32 ), STATUS_SUCCESS );
    CHECK( eq_hex( out, 32, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2" ) );
    CHECK( k32->private[0] != 0 );      /* the state went back to the 32-bit key */
    CHECK_EQ( CALL32( key_symmetric_vector_reset, k32 ), STATUS_SUCCESS );
    CHECK_EQ( CALL32( key_symmetric_destroy, k32 ), STATUS_SUCCESS );
    CHECK_EQ( k32->private[0], 0 );

    /* RSA sign through the 32-bit table, the padding info and its string in guest memory */
    import_rsa1024( &native );
    memset( k32, 0, sizeof(*k32) );
    k32->alg_id = ALG_ID_RSA;
    memcpy( k32->private, native.private, sizeof(native.private) );
    k32->u.a.bitlen = 1024;
    k32->u.a.flags = KEY_FLAG_FINALIZED;
    alg = (WCHAR *)(arena + 1200);
    memcpy( alg, BCRYPT_SHA256_ALGORITHM, 7 * sizeof(WCHAR) );
    pad32 = (ULONG *)(arena + 1280);
    *pad32 = G( alg );
    memcpy( arena + 1300, hash, 32 );
    sig = arena + 2048;
    ret_len = (ULONG *)(arena + 1400);
    sign32 = (void *)(arena + 1500);
    sign32->key = G( k32 ); sign32->padding = G( pad32 ); sign32->input = G( arena + 1300 ); sign32->input_len = 32;
    sign32->output = G( sig ); sign32->output_len = 128; sign32->ret_len = G( ret_len ); sign32->flags = BCRYPT_PAD_PKCS1;
    CHECK_EQ( CALL32( key_asymmetric_sign, sign32 ), STATUS_SUCCESS );
    CHECK_EQ( *ret_len, 128 );
    CHECK_EQ( verify( &native, BCRYPT_PAD_PKCS1, &pkcs, hash, 32, sig, 128 ), STATUS_SUCCESS );
    /* a NULL (0) output pointer stays NULL: the size query */
    sign32->output = 0;
    CHECK_EQ( CALL32( key_asymmetric_sign, sign32 ), STATUS_SUCCESS );
    CHECK_EQ( *ret_len, 128 );
    destroy( &native );
    (void)len;
#undef G
    fake_base = 0;
    CHECK_EQ( fake_ac_live(), 0 );
}

static void test_attach_and_selftest( void )
{
    CHECK_EQ( CALL( process_attach, NULL ), STATUS_SUCCESS );
    CHECK_EQ( CALL( process_attach, NULL ), STATUS_SUCCESS );
    CHECK_EQ( CALL32( process_attach, NULL ), STATUS_SUCCESS );
    CHECK_EQ( CALL( process_detach, NULL ), STATUS_SUCCESS );
    /* each known-answer test of the self-test passes */
    CHECK( selftest_aes( CHAIN_MODE_CBC, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51",
                         "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2" ) );
    CHECK( selftest_aes( CHAIN_MODE_CFB, "6bc1bee22e409f96e93d7e117393172aae2d", "3b79424c9c0dd436bace9e0ed4586a4f32b9" ) );
    CHECK( selftest_gcm() );
    CHECK( selftest_ecdh_p256() );
    CHECK( selftest_x25519() );
    CHECK( selftest_ecdsa() );
    CHECK( selftest_rsa() );
    CHECK_EQ( fake_ac_live(), 0 );
}

int main( void )
{
    test_attach_and_selftest();
    test_aes_cbc_ecb_cfb8();
    test_aes_gcm();
    test_3des_and_unsupported();
    test_rsa_import_export();
    test_rsa_encrypt();
    test_rsa_sign();
    test_rsa_generate();
    test_ecdsa();
    test_rs_der();
    test_ecdh();
    test_x25519();
    test_dh();
    test_dsa();
    test_wow64();
    return ios_test_done( "test_bcrypt" );
}
