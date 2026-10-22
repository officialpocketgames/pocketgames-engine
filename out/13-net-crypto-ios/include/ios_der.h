// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarifications 8.3, 8.4, 11.3: format conversion only, no cryptography.
//   - RSA keys as PKCS #1 DER (RFC 8017 appendix A.1), the form Security.framework takes and gives:
//       RSAPublicKey  ::= SEQUENCE { modulus, publicExponent }
//       RSAPrivateKey ::= SEQUENCE { version (0), modulus, publicExponent, privateExponent,
//                                    prime1, prime2, exponent1, exponent2, coefficient }
//   - an ECDSA signature as Apple gives it, DER SEQUENCE { INTEGER r, INTEGER s }, to r || s with
//     each half left-padded to the curve's size.
// Integers are unsigned big-endian byte strings; leading zero bytes are allowed on input and
// never written on output (apart from the one DER needs before a high bit).

#ifndef MYIOSDECK_IOS_DER_H
#define MYIOSDECK_IOS_DER_H

#include <stddef.h>

struct ios_be
{
    const unsigned char *data;
    size_t len;
};

struct ios_rsa_parts
{
    struct ios_be n, e, d, p, q, dp, dq, qinv;   /* the last six absent (len 0) for a public key */
};

/* PKCS #1 RSAPrivateKey (all eight parts needed) or RSAPublicKey (n, e), malloc'ed; 0 on success */
extern int ios_der_rsa_private( const struct ios_rsa_parts *k, unsigned char **der, size_t *len );
extern int ios_der_rsa_public( const struct ios_rsa_parts *k, unsigned char **der, size_t *len );

/* parse either form; the parts point into der, leading zeros stripped; *is_private says which.
 * 0 on success */
extern int ios_der_rsa_parse( const unsigned char *der, size_t len, struct ios_rsa_parts *k,
                              int *is_private );

/* DER SEQUENCE { INTEGER r, INTEGER s } -> r || s, each half bytes; 0 on success, -1 when the
 * encoding is wrong or a value doesn't fit in half bytes */
extern int ios_der_sig_to_rs( const unsigned char *der, size_t len, unsigned char *out, size_t half );

/* strip leading zero bytes */
extern struct ios_be ios_be_trim( const unsigned char *data, size_t len );

#endif /* MYIOSDECK_IOS_DER_H */
