// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 15: what the host tests can ask the fake of include/ios_applecrypto.h
// (tests/fake_applecrypto.c, built on BearSSL).

#ifndef MYIOSDECK_FAKE_APPLECRYPTO_H
#define MYIOSDECK_FAKE_APPLECRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include "ios_applecrypto.h"

/* objects created and not yet released (cryptors, keys, PKCS #12 results): 0 after a test that
 * released everything */
extern int fake_ac_live( void );

/* calls of each SecKeyAlgorithm (encrypt, decrypt, sign, verify) since the last reset */
extern unsigned int fake_ac_alg_calls( enum ios_ac_alg alg );
/* other routes */
enum fake_ac_route
{
    FAKE_AC_CRYPTOR_CREATE, FAKE_AC_KEY_CREATE, FAKE_AC_KEY_GENERATE, FAKE_AC_KEY_GENERATE_REFUSED,
    FAKE_AC_ECDH, FAKE_AC_TRUST, FAKE_AC_PKCS12, FAKE_AC_RANDOM, FAKE_AC_ROUTES
};
extern unsigned int fake_ac_route_calls( enum fake_ac_route route );
extern void fake_ac_reset_counts( void );

/* the smallest and largest RSA size SecKeyCreateRandomKey accepts in the fake (default 1024..4096) */
extern void fake_ac_set_rsa_generate_range( unsigned int min_bits, unsigned int max_bits );

/* the anchor check: a certificate whose DER contains this byte string is "not trusted"; NULL
 * trusts everything */
extern void fake_ac_set_distrust_marker( const void *marker, size_t len );

/* the next ios_ac_pkcs12_import result: the certificates (deduplicated by the fake as Apple's
 * code is), the identity count, and an RSA private key as PKCS #1 DER or NULL; status != 0 makes
 * the import fail with that OSStatus. The password the fake expects; NULL accepts any. */
extern void fake_ac_set_pkcs12( int32_t status, const uint16_t *password,
                                const unsigned char *const *certs, const size_t *cert_lens,
                                unsigned int cert_count, unsigned int identities,
                                const unsigned char *key_der, size_t key_der_len );

#endif /* MYIOSDECK_FAKE_APPLECRYPTO_H */
