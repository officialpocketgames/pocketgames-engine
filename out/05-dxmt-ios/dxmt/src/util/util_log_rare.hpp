// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05: "log" means one line at the first occurrence and then rarely.
// WARN_RARE logs the 1st, 257th, 513th, ... occurrence of its call site.

#pragma once

#include <atomic>
#include <cstdint>
#include "log/log.hpp"

#define WARN_RARE(...)                                                                                                 \
  do {                                                                                                                 \
    static std::atomic<uint64_t> dxmt_rare_count_{0};                                                                  \
    uint64_t dxmt_rare_n_ = dxmt_rare_count_.fetch_add(1, std::memory_order_relaxed);                                  \
    if (dxmt_rare_n_ % 256 == 0)                                                                                       \
      WARN(__VA_ARGS__, " (occurrence ", dxmt_rare_n_ + 1, ")");                                                       \
  } while (0)
