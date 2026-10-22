// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part E: the check macros the test_sig_*.c programs share. Each program is its own
// executable and has its own counters.

#ifndef MYIOSDECK_TEST_SIG_CHECK_H
#define MYIOSDECK_TEST_SIG_CHECK_H

#include <stdint.h>
#include <stdio.h>

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

/* two unsigned 64-bit values, printed in hex when they differ */
#define CHECK_EQ(a, b) do { uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b); checks++; \
    if (a_ != b_) { failures++; printf( "FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", \
        __FILE__, __LINE__, #a, #b, (unsigned long long)a_, (unsigned long long)b_ ); } } while (0)

static int check_summary(void)
{
    printf( "%d checks, %d failed\n", checks, failures );
    return failures ? 1 : 0;
}

#endif /* MYIOSDECK_TEST_SIG_CHECK_H */
