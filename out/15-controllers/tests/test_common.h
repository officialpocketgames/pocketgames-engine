// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part F: a tiny check harness shared by the host tests.

#ifndef MYIOSDECK_TEST_COMMON_H
#define MYIOSDECK_TEST_COMMON_H

#include <stdio.h>
#include <string.h>

static int test_failures;
static int test_checks;

#define CHECK(cond) do { \
    test_checks++; \
    if (!(cond)) { test_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long check_a_ = (long long)(a), check_b_ = (long long)(b); \
    test_checks++; \
    if (check_a_ != check_b_) { test_failures++; \
        fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, check_a_, check_b_); } \
} while (0)

#define CHECK_STR(a, b) do { \
    const char *check_a_ = (a), *check_b_ = (b); \
    test_checks++; \
    if (strcmp(check_a_, check_b_)) { test_failures++; \
        fprintf(stderr, "%s:%d: CHECK_STR failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, check_a_, check_b_); } \
} while (0)

static int test_report(const char *name)
{
    if (test_failures) fprintf(stderr, "%s: %d of %d checks FAILED\n", name, test_failures, test_checks);
    else printf("%s: all %d checks passed\n", name, test_checks);
    return test_failures ? 1 : 0;
}

#endif
