// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: a minimal check harness for the host tests.

#ifndef TEST_CHECK_H
#define TEST_CHECK_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failures, test_checks;

#define CHECK( cond ) do { \
    test_checks++; \
    if (!(cond)) { test_failures++; fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); } \
} while (0)

#define CHECK_INT( a, b ) do { \
    long long va_ = (long long)(a), vb_ = (long long)(b); \
    test_checks++; \
    if (va_ != vb_) { test_failures++; fprintf( stderr, "%s:%d: %s == %lld, expected %s == %lld\n", \
                                                 __FILE__, __LINE__, #a, va_, #b, vb_ ); } \
} while (0)

#define CHECK_NEAR( a, b, eps ) do { \
    double va_ = (double)(a), vb_ = (double)(b); \
    test_checks++; \
    if (!(fabs( va_ - vb_ ) <= (eps))) { test_failures++; fprintf( stderr, "%s:%d: %s == %g, expected %g (+-%g)\n", \
                                                 __FILE__, __LINE__, #a, va_, vb_, (double)(eps) ); } \
} while (0)

#define CHECK_MEM( a, b, n ) do { \
    test_checks++; \
    if (memcmp( (a), (b), (n) )) { test_failures++; fprintf( stderr, "%s:%d: %s differs from %s (%zu bytes)\n", \
                                                 __FILE__, __LINE__, #a, #b, (size_t)(n) ); } \
} while (0)

static inline int test_report( const char *name )
{
    printf( "%s: %d checks, %d failures\n", name, test_checks, test_failures );
    return test_failures ? 1 : 0;
}

#endif /* TEST_CHECK_H */
