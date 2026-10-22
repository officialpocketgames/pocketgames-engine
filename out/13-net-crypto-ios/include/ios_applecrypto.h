// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 15: every call into Apple's Security.framework and CommonCrypto goes
// through this header. On iOS it is common/ios_applecrypto.c; the host tests link
// tests/fake_applecrypto.c instead (built on BearSSL, counting calls per route and the
// CF-style creates and releases so a test can check nothing leaks).
//
// No CF type appears here, so the callers (bcrypt, secur32, crypt32) compile the same way on
// both sides. Every function that returns a buffer allocates it with malloc; the caller frees
// it. Every object returned by a *_create / *_generate / *_copy_* / *_import function is the
// caller's and is given back with the matching *_release / *_free.
//
// Return values: IOS_AC_OK (0) on success; IOS_AC_FAIL on failure; IOS_AC_UNSUPPORTED where the
// framework refuses the parameters themselves (for example an RSA key size), so the caller can
// take another route.

#ifndef MYIOSDECK_IOS_APPLECRYPTO_H
#define MYIOSDECK_IOS_APPLECRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define IOS_AC_OK           0
#define IOS_AC_FAIL        (-1)
#define IOS_AC_UNSUPPORTED (-2)

/* ---------------------------------------------------------------- randomness */

/* SecRandomCopyBytes( kSecRandomDefault, len, buf ) */
extern int ios_ac_random( void *buf, size_t len );

/* ---------------------------------------------------------------- CommonCrypto */

enum ios_ac_cipher { IOS_AC_AES, IOS_AC_3DES };
enum ios_ac_mode   { IOS_AC_ECB, IOS_AC_CBC, IOS_AC_CFB8 };

typedef struct ios_ac_cryptor ios_ac_cryptor;

/* CCCryptorCreateWithMode( encrypt ? kCCEncrypt : kCCDecrypt, mode, cipher, ccNoPadding, iv,
 * key, key_len, NULL, 0, 0, 0, &cryptor ). iv is the block size (16 or 8 bytes) or NULL (ECB,
 * or a zero IV). */
extern int ios_ac_cryptor_create( int encrypt, enum ios_ac_cipher cipher, enum ios_ac_mode mode,
                                  const void *key, size_t key_len, const void *iv,
                                  ios_ac_cryptor **ret );
/* CCCryptorUpdate: exactly len bytes in, len bytes out (no padding, whole blocks for ECB/CBC) */
extern int ios_ac_cryptor_update( ios_ac_cryptor *cryptor, const void *in, size_t len, void *out );
extern void ios_ac_cryptor_release( ios_ac_cryptor *cryptor );

/* ---------------------------------------------------------------- SecKey */

enum ios_ac_key_type { IOS_AC_KEY_RSA, IOS_AC_KEY_EC };

typedef struct ios_ac_key ios_ac_key;   /* a SecKeyRef on iOS */

/* SecKeyCreateWithData: RSA keys as PKCS #1 DER (RSAPrivateKey / RSAPublicKey), EC keys as ANSI
 * X9.63 (04 || X || Y [|| D]); never kSecAttrIsPermanent */
extern int ios_ac_key_create( enum ios_ac_key_type type, int is_private, const unsigned char *data,
                              size_t len, ios_ac_key **ret );
/* SecKeyCreateRandomKey with kSecAttrKeySizeInBits = bits (EC: 256, 384, 521) */
extern int ios_ac_key_generate( enum ios_ac_key_type type, unsigned int bits, ios_ac_key **ret );
/* SecKeyCopyPublicKey */
extern int ios_ac_key_copy_public( ios_ac_key *key, ios_ac_key **ret );
/* SecKeyCopyExternalRepresentation: the same formats as ios_ac_key_create */
extern int ios_ac_key_export( ios_ac_key *key, unsigned char **data, size_t *len );
/* SecKeyCopyAttributes: kSecAttrKeyType and kSecAttrKeySizeInBits */
extern int ios_ac_key_info( ios_ac_key *key, enum ios_ac_key_type *type, unsigned int *bits );
/* CFRetain / CFRelease */
extern ios_ac_key *ios_ac_key_retain( ios_ac_key *key );
extern void ios_ac_key_release( ios_ac_key *key );

/* the SecKeyAlgorithm a call uses */
enum ios_ac_alg
{
    IOS_AC_RSA_ENC_PKCS1,            /* kSecKeyAlgorithmRSAEncryptionPKCS1 */
    IOS_AC_RSA_ENC_RAW,              /* kSecKeyAlgorithmRSAEncryptionRaw */
    IOS_AC_RSA_ENC_OAEP_SHA1,        /* kSecKeyAlgorithmRSAEncryptionOAEPSHA1 */
    IOS_AC_RSA_ENC_OAEP_SHA256,
    IOS_AC_RSA_ENC_OAEP_SHA384,
    IOS_AC_RSA_ENC_OAEP_SHA512,
    IOS_AC_RSA_SIG_PKCS1_RAW,        /* kSecKeyAlgorithmRSASignatureDigestPKCS1v15Raw */
    IOS_AC_RSA_SIG_PKCS1_SHA1,       /* kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1 */
    IOS_AC_RSA_SIG_PKCS1_SHA256,
    IOS_AC_RSA_SIG_PKCS1_SHA384,
    IOS_AC_RSA_SIG_PKCS1_SHA512,
    IOS_AC_RSA_SIG_PSS_SHA1,         /* kSecKeyAlgorithmRSASignatureDigestPSSSHA1 */
    IOS_AC_RSA_SIG_PSS_SHA256,
    IOS_AC_RSA_SIG_PSS_SHA384,
    IOS_AC_RSA_SIG_PSS_SHA512,
    IOS_AC_ECDSA_DIGEST_X962,        /* kSecKeyAlgorithmECDSASignatureDigestX962 (DER signature) */
    IOS_AC_ALG_COUNT
};

/* SecKeyCreateEncryptedData / SecKeyCreateDecryptedData */
extern int ios_ac_encrypt( ios_ac_key *key, enum ios_ac_alg alg, const void *in, size_t len,
                           unsigned char **out, size_t *out_len );
extern int ios_ac_decrypt( ios_ac_key *key, enum ios_ac_alg alg, const void *in, size_t len,
                           unsigned char **out, size_t *out_len );
/* SecKeyCreateSignature over a digest */
extern int ios_ac_sign( ios_ac_key *key, enum ios_ac_alg alg, const void *digest, size_t len,
                        unsigned char **sig, size_t *sig_len );
/* SecKeyVerifySignature: IOS_AC_OK if valid, IOS_AC_FAIL otherwise */
extern int ios_ac_verify( ios_ac_key *key, enum ios_ac_alg alg, const void *digest, size_t len,
                          const void *sig, size_t sig_len );
/* SecKeyCopyKeyExchangeResult( priv, kSecKeyAlgorithmECDHKeyExchangeStandard, pub, {} ):
 * the shared x-coordinate */
extern int ios_ac_ecdh( ios_ac_key *priv, ios_ac_key *pub, unsigned char **out, size_t *out_len );

/* ---------------------------------------------------------------- certificates */

/* SecCertificateCreateWithData, SecPolicyCreateBasicX509, SecTrustCreateWithCertificates,
 * SecTrustSetNetworkFetchAllowed( false ), SecTrustEvaluateWithError: 1 when the device trusts
 * the certificate as an anchor, 0 when it doesn't, IOS_AC_FAIL when it isn't a certificate */
extern int ios_ac_cert_is_trusted_anchor( const unsigned char *der, size_t len );

/* ---------------------------------------------------------------- PKCS #12 */

struct ios_ac_p12
{
    unsigned int    cert_count;     /* certificates of every item's chain, in order, deduplicated */
    unsigned char **certs;          /* DER, malloc'ed */
    size_t         *cert_lens;
    unsigned int    identities;     /* items with an identity */
    ios_ac_key     *key;            /* the first identity's private key, or NULL */
};

/* SecPKCS12Import( pfx, { kSecImportExportPassphrase: password }, &items ). password is the
 * caller's UTF-16 string (NUL-terminated), NULL meaning "". On failure *status is the OSStatus. */
extern int ios_ac_pkcs12_import( const void *pfx, size_t len, const uint16_t *password,
                                 struct ios_ac_p12 **ret, int32_t *status );
extern void ios_ac_pkcs12_free( struct ios_ac_p12 *p12 );

#endif /* MYIOSDECK_IOS_APPLECRYPTO_H */
