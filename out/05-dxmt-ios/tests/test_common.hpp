// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Minimal test helpers for the spec 05 host tests (no framework needed).

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failures++;                                                                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                      \
    }                                                                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    long long va_ = (long long)(a), vb_ = (long long)(b);                                                              \
    if (va_ != vb_) {                                                                                                  \
      g_failures++;                                                                                                    \
      std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_);                      \
    }                                                                                                                  \
  } while (0)

/* Compare 4 bytes of an RGBA8 texel. */
#define CHECK_RGBA(texel, r, g, b, a)                                                                                  \
  do {                                                                                                                 \
    const uint8_t *t_ = (texel);                                                                                       \
    CHECK_EQ(t_[0], r);                                                                                                \
    CHECK_EQ(t_[1], g);                                                                                                \
    CHECK_EQ(t_[2], b);                                                                                                \
    CHECK_EQ(t_[3], a);                                                                                                \
  } while (0)

inline int
TestSummary(const char *name) {
  std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
  return g_failures ? 1 : 0;
}
