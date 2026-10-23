// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 8.4 (DH, DSA, RSAPRIVATE completion) and 8.7 (LibTomCrypt's PRNG):
// LibTomCrypt and LibTomMath behind byte strings, so bcrypt never includes their headers.
// Implemented in common/ios_ltc.c. Integers are unsigned big-endian; every output buffer is
// malloc'ed and freed by the caller. Each function returns 0 on success and -1 on failure.

#ifndef MYIOSDECK_IOS_LTC_H
#define MYIOSDECK_IOS_LTC_H

#include <stddef.h>

struct ios_buf
{
    unsigned char *data;    /* malloc'ed, minimal (no leading zero byte) */
    size_t len;
};

/* the one-time set-up: ltc_mp = ltm_desc, the SHA-1 and SHA-2 hashes, the PRNG whose read is
 * SecRandomCopyBytes (ios_ac_random). Idempotent and thread-safe. */
extern int ios_ltc_init( void );

/* RSA: d = e^-1 mod (p-1)(q-1), dp = d mod (p-1), dq = d mod (q-1), qinv = q^-1 mod p */
extern int ios_ltc_rsa_complete( const unsigned char *e, size_t elen, const unsigned char *p, size_t plen,
                                 const unsigned char *q, size_t qlen, struct ios_buf *d, struct ios_buf *dp,
                                 struct ios_buf *dq, struct ios_buf *qinv );

/* DH: the MODP group of exactly bits (768 1024 1536 2048 3072 4096 6144 8192), else p and g of
 * dsa_generate_pqg (modulus bits/8 bytes, group 32 bytes) */
extern int ios_ltc_dh_params( unsigned int bits, int *from_group, struct ios_buf *p, struct ios_buf *g );
extern int ios_ltc_dh_generate( const unsigned char *p, size_t plen, const unsigned char *g, size_t glen,
                                struct ios_buf *y, struct ios_buf *x );
extern int ios_ltc_dh_public( const unsigned char *p, size_t plen, const unsigned char *g, size_t glen,
                              const unsigned char *x, size_t xlen, struct ios_buf *y );
extern int ios_ltc_dh_shared( const unsigned char *p, size_t plen, const unsigned char *g, size_t glen,
                              const unsigned char *x, size_t xlen, const unsigned char *peer_y, size_t ylen,
                              struct ios_buf *secret );

/* DSA with a 20-byte q */
extern int ios_ltc_dsa_generate( unsigned int bits, struct ios_buf *p, struct ios_buf *q, struct ios_buf *g,
                                 struct ios_buf *y, struct ios_buf *x );
extern int ios_ltc_dsa_public( const unsigned char *p, size_t plen, const unsigned char *q, size_t qlen,
                               const unsigned char *g, size_t glen, const unsigned char *x, size_t xlen,
                               struct ios_buf *y );
/* r and s left-padded into 20 bytes each */
extern int ios_ltc_dsa_sign( const unsigned char *p, size_t plen, const unsigned char *q, size_t qlen,
                             const unsigned char *g, size_t glen, const unsigned char *x, size_t xlen,
                             const unsigned char *hash, size_t hashlen, unsigned char r[20], unsigned char s[20] );
/* 1 valid, 0 invalid, -1 error */
extern int ios_ltc_dsa_verify( const unsigned char *p, size_t plen, const unsigned char *q, size_t qlen,
                               const unsigned char *g, size_t glen, const unsigned char *y, size_t ylen,
                               const unsigned char *hash, size_t hashlen, const unsigned char *r, size_t rlen,
                               const unsigned char *s, size_t slen );

extern void ios_buf_free( struct ios_buf *b );

#endif /* MYIOSDECK_IOS_LTC_H */
