// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/* Minimal check macros for the spec 18 host tests. */

#ifndef MYIOSDECK_TEST_CHECK_H
#define MYIOSDECK_TEST_CHECK_H

#include <stdio.h>

static int check_failures;
static int check_count;

#define CHECK(cond) do { \
        check_count++; \
        if (!(cond)) { check_failures++; fprintf( stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond ); } \
    } while (0)

#define CHECK_EQ(a, b) do { \
        unsigned long long check_a_ = (unsigned long long)(a), check_b_ = (unsigned long long)(b); \
        check_count++; \
        if (check_a_ != check_b_) { check_failures++; \
            fprintf( stderr, "%s:%d: FAILED: %s == %s (%#llx != %#llx)\n", __FILE__, __LINE__, #a, #b, check_a_, check_b_ ); } \
    } while (0)

static inline int check_report( const char *name )
{
    printf( "%s: %d checks, %d failed\n", name, check_count, check_failures );
    return check_failures ? 1 : 0;
}

#endif
