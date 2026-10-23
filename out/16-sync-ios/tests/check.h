// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part J: the tests' check macro.

#ifndef IOS_SYNC_TEST_CHECK_H
#define IOS_SYNC_TEST_CHECK_H

#include <stdio.h>

static int check_failures;

#define CHECK(cond) do { \
    if (!(cond)) { check_failures++; fprintf( stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond ); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long check_a_ = (long long)(a), check_b_ = (long long)(b); \
    if (check_a_ != check_b_) { check_failures++; \
        fprintf( stderr, "%s:%d: check failed: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, check_a_, check_b_ ); } \
} while (0)

static inline int check_done( const char *name )
{
    if (check_failures) fprintf( stderr, "%s: %d failure(s)\n", name, check_failures );
    else printf( "%s: ok\n", name );
    return check_failures != 0;
}

#endif
