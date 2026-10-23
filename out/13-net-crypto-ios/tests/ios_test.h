// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Minimal check macros for the spec 13 host tests. Each test program returns 0 when every
// CHECK passed and 1 otherwise, and prints one line per failure.

#ifndef MYIOSDECK_IOS_TEST_H
#define MYIOSDECK_IOS_TEST_H

#include <stdio.h>
#include <string.h>

static int ios_test_failures;
static int ios_test_checks;

#define CHECK(cond) do { \
    ios_test_checks++; \
    if (!(cond)) { ios_test_failures++; \
        printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    unsigned long long ios_a_ = (unsigned long long)(a), ios_b_ = (unsigned long long)(b); \
    ios_test_checks++; \
    if (ios_a_ != ios_b_) { ios_test_failures++; \
        printf( "FAIL %s:%d: %s == %s (%#llx != %#llx)\n", __FILE__, __LINE__, #a, #b, ios_a_, ios_b_ ); } \
} while (0)

#define CHECK_STR(a, b) do { \
    const char *ios_a_ = (a), *ios_b_ = (b); \
    ios_test_checks++; \
    if (!ios_a_ || !ios_b_ || strcmp( ios_a_, ios_b_ )) { ios_test_failures++; \
        printf( "FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__, __LINE__, #a, \
                ios_b_ ? ios_b_ : "(null)", ios_a_ ? ios_a_ : "(null)" ); } \
} while (0)

static inline int ios_test_done( const char *name )
{
    printf( "%s: %d checks, %d failed\n", name, ios_test_checks, ios_test_failures );
    return ios_test_failures ? 1 : 0;
}

#endif /* MYIOSDECK_IOS_TEST_H */
