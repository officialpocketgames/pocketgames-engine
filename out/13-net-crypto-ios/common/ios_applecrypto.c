// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarifications 8, 9, 11, 12: include/ios_applecrypto.h implemented with Apple's public
// Security.framework and CommonCrypto (iOS 17 SDK). Everything works in memory on the calling
// thread: no keychain item is created, read or deleted (no kSecAttrIsPermanent, no keychain
// option to SecPKCS12Import), no entitlement or access group is needed, and the trust check
// never fetches from the network. Every CF object made here is released on every path.
//
// The host tests use tests/fake_applecrypto.c instead of this file.

#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <CommonCrypto/CommonCryptor.h>

#include "ios_applecrypto.h"

/* ---------------------------------------------------------------- helpers */

static void release_cf( CFTypeRef obj )
{
    if (obj) CFRelease( obj );
}

/* a malloc'ed copy of a CFData; IOS_AC_FAIL if there is none */
static int copy_cfdata( CFDataRef data, unsigned char **out, size_t *out_len )
{
    CFIndex len;

    if (!data) return IOS_AC_FAIL;
    len = CFDataGetLength( data );
    if (!(*out = malloc( len ? len : 1 ))) return IOS_AC_FAIL;
    memcpy( *out, CFDataGetBytePtr( data ), len );
    *out_len = len;
    return IOS_AC_OK;
}

/* ---------------------------------------------------------------- randomness */

int ios_ac_random( void *buf, size_t len )
{
    if (!len) return IOS_AC_OK;
    return SecRandomCopyBytes( kSecRandomDefault, len, buf ) == errSecSuccess ? IOS_AC_OK : IOS_AC_FAIL;
}

/* ---------------------------------------------------------------- CommonCrypto */

struct ios_ac_cryptor
{
    CCCryptorRef ref;
};

int ios_ac_cryptor_create( int encrypt, enum ios_ac_cipher cipher, enum ios_ac_mode mode,
                           const void *key, size_t key_len, const void *iv, ios_ac_cryptor **ret )
{
    CCAlgorithm alg = cipher == IOS_AC_AES ? kCCAlgorithmAES : kCCAlgorithm3DES;
    CCMode ccmode;
    struct ios_ac_cryptor *c;

    switch (mode)
    {
    case IOS_AC_ECB:  ccmode = kCCModeECB; iv = NULL; break;
    case IOS_AC_CBC:  ccmode = kCCModeCBC; break;
    case IOS_AC_CFB8: ccmode = kCCModeCFB8; break;
    default: return IOS_AC_UNSUPPORTED;
    }
    if (!(c = calloc( 1, sizeof(*c) ))) return IOS_AC_FAIL;
    if (CCCryptorCreateWithMode( encrypt ? kCCEncrypt : kCCDecrypt, ccmode, alg, ccNoPadding, iv,
                                 key, key_len, NULL, 0, 0, 0, &c->ref ) != kCCSuccess)
    {
        free( c );
        return IOS_AC_FAIL;
    }
    *ret = c;
    return IOS_AC_OK;
}

int ios_ac_cryptor_update( ios_ac_cryptor *c, const void *in, size_t len, void *out )
{
    size_t moved = 0;

    if (!len) return IOS_AC_OK;
    if (CCCryptorUpdate( c->ref, in, len, out, len, &moved ) != kCCSuccess || moved != len)
        return IOS_AC_FAIL;
    return IOS_AC_OK;
}

void ios_ac_cryptor_release( ios_ac_cryptor *c )
{
    if (!c) return;
    CCCryptorRelease( c->ref );
    free( c );
}

/* ---------------------------------------------------------------- SecKey */

/* an ios_ac_key is the SecKeyRef itself */
static SecKeyRef seckey( ios_ac_key *key ) { return (SecKeyRef)key; }

static CFStringRef key_type_attr( enum ios_ac_key_type type )
{
    return type == IOS_AC_KEY_RSA ? kSecAttrKeyTypeRSA : kSecAttrKeyTypeECSECPrimeRandom;
}

int ios_ac_key_create( enum ios_ac_key_type type, int is_private, const unsigned char *data,
                       size_t len, ios_ac_key **ret )
{
    const void *keys[2] = { kSecAttrKeyType, kSecAttrKeyClass };
    const void *values[2] = { key_type_attr( type ), is_private ? kSecAttrKeyClassPrivate : kSecAttrKeyClassPublic };
    CFDictionaryRef attrs;
    CFDataRef cfdata;
    CFErrorRef error = NULL;
    SecKeyRef key;

    attrs = CFDictionaryCreate( NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks );
    cfdata = CFDataCreate( NULL, data, len );
    key = (attrs && cfdata) ? SecKeyCreateWithData( cfdata, attrs, &error ) : NULL;
    release_cf( cfdata );
    release_cf( attrs );
    release_cf( error );
    if (!key) return IOS_AC_FAIL;
    *ret = (ios_ac_key *)key;
    return IOS_AC_OK;
}

int ios_ac_key_generate( enum ios_ac_key_type type, unsigned int bits, ios_ac_key **ret )
{
    const void *keys[2] = { kSecAttrKeyType, kSecAttrKeySizeInBits };
    const void *values[2];
    int ibits = bits;
    CFNumberRef size = CFNumberCreate( NULL, kCFNumberIntType, &ibits );
    CFDictionaryRef attrs;
    CFErrorRef error = NULL;
    SecKeyRef key = NULL;

    values[0] = key_type_attr( type );
    values[1] = size;
    attrs = size ? CFDictionaryCreate( NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks ) : NULL;
    if (attrs) key = SecKeyCreateRandomKey( attrs, &error );
    release_cf( attrs );
    release_cf( size );
    release_cf( error );
    /* a refused size is the case the caller has another route for */
    if (!key) return IOS_AC_UNSUPPORTED;
    *ret = (ios_ac_key *)key;
    return IOS_AC_OK;
}

int ios_ac_key_copy_public( ios_ac_key *key, ios_ac_key **ret )
{
    SecKeyRef pub = SecKeyCopyPublicKey( seckey( key ) );

    if (!pub) return IOS_AC_FAIL;
    *ret = (ios_ac_key *)pub;
    return IOS_AC_OK;
}

int ios_ac_key_export( ios_ac_key *key, unsigned char **data, size_t *len )
{
    CFErrorRef error = NULL;
    CFDataRef rep = SecKeyCopyExternalRepresentation( seckey( key ), &error );
    int ret = copy_cfdata( rep, data, len );

    release_cf( rep );
    release_cf( error );
    return ret;
}

int ios_ac_key_info( ios_ac_key *key, enum ios_ac_key_type *type, unsigned int *bits )
{
    CFDictionaryRef attrs = SecKeyCopyAttributes( seckey( key ) );
    CFTypeRef t;
    CFNumberRef n;
    int ibits = 0, ret = IOS_AC_FAIL;

    if (!attrs) return IOS_AC_FAIL;
    t = CFDictionaryGetValue( attrs, kSecAttrKeyType );
    n = CFDictionaryGetValue( attrs, kSecAttrKeySizeInBits );
    if (t && n && CFGetTypeID( n ) == CFNumberGetTypeID() && CFNumberGetValue( n, kCFNumberIntType, &ibits ))
    {
        if (CFEqual( t, kSecAttrKeyTypeRSA )) { *type = IOS_AC_KEY_RSA; ret = IOS_AC_OK; }
        else if (CFEqual( t, kSecAttrKeyTypeECSECPrimeRandom )) { *type = IOS_AC_KEY_EC; ret = IOS_AC_OK; }
        *bits = ibits;
    }
    CFRelease( attrs );
    return ret;
}

ios_ac_key *ios_ac_key_retain( ios_ac_key *key )
{
    if (key) CFRetain( seckey( key ) );
    return key;
}

void ios_ac_key_release( ios_ac_key *key )
{
    if (key) CFRelease( seckey( key ) );
}

static SecKeyAlgorithm sec_alg( enum ios_ac_alg alg )
{
    switch (alg)
    {
    case IOS_AC_RSA_ENC_PKCS1:        return kSecKeyAlgorithmRSAEncryptionPKCS1;
    case IOS_AC_RSA_ENC_RAW:          return kSecKeyAlgorithmRSAEncryptionRaw;
    case IOS_AC_RSA_ENC_OAEP_SHA1:    return kSecKeyAlgorithmRSAEncryptionOAEPSHA1;
    case IOS_AC_RSA_ENC_OAEP_SHA256:  return kSecKeyAlgorithmRSAEncryptionOAEPSHA256;
    case IOS_AC_RSA_ENC_OAEP_SHA384:  return kSecKeyAlgorithmRSAEncryptionOAEPSHA384;
    case IOS_AC_RSA_ENC_OAEP_SHA512:  return kSecKeyAlgorithmRSAEncryptionOAEPSHA512;
    case IOS_AC_RSA_SIG_PKCS1_RAW:    return kSecKeyAlgorithmRSASignatureDigestPKCS1v15Raw;
    case IOS_AC_RSA_SIG_PKCS1_SHA1:   return kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1;
    case IOS_AC_RSA_SIG_PKCS1_SHA256: return kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256;
    case IOS_AC_RSA_SIG_PKCS1_SHA384: return kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA384;
    case IOS_AC_RSA_SIG_PKCS1_SHA512: return kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512;
    case IOS_AC_RSA_SIG_PSS_SHA1:     return kSecKeyAlgorithmRSASignatureDigestPSSSHA1;
    case IOS_AC_RSA_SIG_PSS_SHA256:   return kSecKeyAlgorithmRSASignatureDigestPSSSHA256;
    case IOS_AC_RSA_SIG_PSS_SHA384:   return kSecKeyAlgorithmRSASignatureDigestPSSSHA384;
    case IOS_AC_RSA_SIG_PSS_SHA512:   return kSecKeyAlgorithmRSASignatureDigestPSSSHA512;
    case IOS_AC_ECDSA_DIGEST_X962:    return kSecKeyAlgorithmECDSASignatureDigestX962;
    default:                          return NULL;
    }
}

/* run one of the four data-in, data-out SecKey calls */
enum seckey_op { OP_ENCRYPT, OP_DECRYPT, OP_SIGN };

static int seckey_data_op( enum seckey_op op, ios_ac_key *key, enum ios_ac_alg alg,
                           const void *in, size_t len, unsigned char **out, size_t *out_len )
{
    SecKeyAlgorithm a = sec_alg( alg );
    CFErrorRef error = NULL;
    CFDataRef input, result = NULL;
    int ret;

    if (!a) return IOS_AC_UNSUPPORTED;
    if (!(input = CFDataCreate( NULL, in, len ))) return IOS_AC_FAIL;
    switch (op)
    {
    case OP_ENCRYPT: result = SecKeyCreateEncryptedData( seckey( key ), a, input, &error ); break;
    case OP_DECRYPT: result = SecKeyCreateDecryptedData( seckey( key ), a, input, &error ); break;
    case OP_SIGN:    result = SecKeyCreateSignature( seckey( key ), a, input, &error ); break;
    }
    ret = copy_cfdata( result, out, out_len );
    release_cf( result );
    release_cf( input );
    release_cf( error );
    return ret;
}

int ios_ac_encrypt( ios_ac_key *key, enum ios_ac_alg alg, const void *in, size_t len,
                    unsigned char **out, size_t *out_len )
{
    return seckey_data_op( OP_ENCRYPT, key, alg, in, len, out, out_len );
}

int ios_ac_decrypt( ios_ac_key *key, enum ios_ac_alg alg, const void *in, size_t len,
                    unsigned char **out, size_t *out_len )
{
    return seckey_data_op( OP_DECRYPT, key, alg, in, len, out, out_len );
}

int ios_ac_sign( ios_ac_key *key, enum ios_ac_alg alg, const void *digest, size_t len,
                 unsigned char **sig, size_t *sig_len )
{
    return seckey_data_op( OP_SIGN, key, alg, digest, len, sig, sig_len );
}

int ios_ac_verify( ios_ac_key *key, enum ios_ac_alg alg, const void *digest, size_t len,
                   const void *sig, size_t sig_len )
{
    SecKeyAlgorithm a = sec_alg( alg );
    CFDataRef d, s;
    CFErrorRef error = NULL;
    Boolean ok = false;

    if (!a) return IOS_AC_FAIL;
    d = CFDataCreate( NULL, digest, len );
    s = CFDataCreate( NULL, sig, sig_len );
    if (d && s) ok = SecKeyVerifySignature( seckey( key ), a, d, s, &error );
    release_cf( d );
    release_cf( s );
    release_cf( error );
    return ok ? IOS_AC_OK : IOS_AC_FAIL;
}

int ios_ac_ecdh( ios_ac_key *priv, ios_ac_key *pub, unsigned char **out, size_t *out_len )
{
    CFDictionaryRef params = CFDictionaryCreate( NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks );
    CFErrorRef error = NULL;
    CFDataRef result = NULL;
    int ret;

    if (params)
        result = SecKeyCopyKeyExchangeResult( seckey( priv ), kSecKeyAlgorithmECDHKeyExchangeStandard,
                                              seckey( pub ), params, &error );
    ret = copy_cfdata( result, out, out_len );
    release_cf( result );
    release_cf( params );
    release_cf( error );
    return ret;
}

/* ---------------------------------------------------------------- certificates */

int ios_ac_cert_is_trusted_anchor( const unsigned char *der, size_t len )
{
    CFDataRef data = CFDataCreate( NULL, der, len );
    SecCertificateRef cert = data ? SecCertificateCreateWithData( NULL, data ) : NULL;
    SecPolicyRef policy = NULL;
    SecTrustRef trust = NULL;
    CFErrorRef error = NULL;
    int ret = IOS_AC_FAIL;

    if (cert && (policy = SecPolicyCreateBasicX509()) &&
        SecTrustCreateWithCertificates( cert, policy, &trust ) == errSecSuccess)
    {
        SecTrustSetNetworkFetchAllowed( trust, false );
        ret = SecTrustEvaluateWithError( trust, &error ) ? 1 : 0;
    }
    release_cf( error );
    release_cf( trust );
    release_cf( policy );
    release_cf( cert );
    release_cf( data );
    return ret;
}

/* ---------------------------------------------------------------- PKCS #12 */

static int p12_add_cert( struct ios_ac_p12 *p12, SecCertificateRef cert )
{
    CFDataRef data = SecCertificateCopyData( cert );
    const UInt8 *bytes;
    CFIndex len;
    unsigned int i;
    void *p;

    if (!data) return IOS_AC_FAIL;
    bytes = CFDataGetBytePtr( data );
    len = CFDataGetLength( data );
    for (i = 0; i < p12->cert_count; i++)   /* duplicates (same DER) removed */
        if (p12->cert_lens[i] == (size_t)len && !memcmp( p12->certs[i], bytes, len ))
        {
            CFRelease( data );
            return IOS_AC_OK;
        }
    if (!(p = realloc( p12->certs, (p12->cert_count + 1) * sizeof(*p12->certs) ))) goto fail;
    p12->certs = p;
    if (!(p = realloc( p12->cert_lens, (p12->cert_count + 1) * sizeof(*p12->cert_lens) ))) goto fail;
    p12->cert_lens = p;
    if (!(p12->certs[p12->cert_count] = malloc( len ? len : 1 ))) goto fail;
    memcpy( p12->certs[p12->cert_count], bytes, len );
    p12->cert_lens[p12->cert_count++] = len;
    CFRelease( data );
    return IOS_AC_OK;
fail:
    CFRelease( data );
    return IOS_AC_FAIL;
}

int ios_ac_pkcs12_import( const void *pfx, size_t len, const uint16_t *password,
                          struct ios_ac_p12 **ret, int32_t *status )
{
    CFStringRef pw;
    CFDataRef data;
    CFDictionaryRef options = NULL;
    CFArrayRef items = NULL;
    struct ios_ac_p12 *p12;
    size_t pwlen = 0;
    OSStatus err;
    CFIndex i, j;

    static const UniChar empty[1] = { 0 };

    *status = 0;
    if (password) while (password[pwlen]) pwlen++;
    pw = CFStringCreateWithCharacters( NULL, password ? (const UniChar *)password : empty, pwlen );
    data = CFDataCreate( NULL, pfx, len );
    if (pw && data)
    {
        const void *k[1] = { kSecImportExportPassphrase };
        const void *v[1] = { pw };
        options = CFDictionaryCreate( NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks );
    }
    err = options ? SecPKCS12Import( data, options, &items ) : errSecAllocate;
    release_cf( options );
    release_cf( data );
    release_cf( pw );
    if (err != errSecSuccess || !items)
    {
        *status = err;
        release_cf( items );
        return IOS_AC_FAIL;
    }

    if (!(p12 = calloc( 1, sizeof(*p12) ))) { CFRelease( items ); return IOS_AC_FAIL; }
    for (i = 0; i < CFArrayGetCount( items ); i++)
    {
        CFDictionaryRef item = CFArrayGetValueAtIndex( items, i );
        CFArrayRef chain = CFDictionaryGetValue( item, kSecImportItemCertChain );
        SecIdentityRef identity = (SecIdentityRef)CFDictionaryGetValue( item, kSecImportItemIdentity );

        if (chain)
            for (j = 0; j < CFArrayGetCount( chain ); j++)
                if (p12_add_cert( p12, (SecCertificateRef)CFArrayGetValueAtIndex( chain, j ) ))
                {
                    CFRelease( items );
                    ios_ac_pkcs12_free( p12 );
                    return IOS_AC_FAIL;
                }
        if (identity)
        {
            if (!p12->identities)
            {
                SecKeyRef key = NULL;
                if (SecIdentityCopyPrivateKey( identity, &key ) == errSecSuccess) p12->key = (ios_ac_key *)key;
            }
            p12->identities++;
        }
    }
    CFRelease( items );
    *ret = p12;
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
}
