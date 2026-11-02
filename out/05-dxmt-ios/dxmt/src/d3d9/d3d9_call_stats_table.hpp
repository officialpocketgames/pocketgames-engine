// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 3 (call statistics): the counter table and its report
// line, free of Windows, Metal and device types so the host test
// (tests/test_d3d9_call_stats.cpp) builds it as it is. d3d9_call_stats.{hpp,cpp}
// own the one table the frontend counts into, the switch, the clock and the log.

#pragma once

#include "d3d9_call_list.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dxmt::d9cs {

/* One id per counted (interface, method) pair, in D9_CALLS order. */
enum D9CallId : uint16_t {
#define D9CS_ID(label, vtable, method) D9C_##label##_##method,
  D9_CALLS(D9CS_ID)
#undef D9CS_ID
  kD9CallCount
};

/* "<interface>::<method>" for every id. */
inline const char *const kD9CallNames[kD9CallCount] = {
#define D9CS_NAME(label, vtable, method) #label "::" #method,
    D9_CALLS(D9CS_NAME)
#undef D9CS_NAME
};

/* A report is due at the first Present this long after the period started. */
constexpr uint64_t kReportPeriodNs = 10ull * 1000 * 1000 * 1000;
/* The report lists this many of the busiest pairs. */
constexpr size_t kReportTop = 16;

/* The report line for one period of `elapsed_ns`:
     d3d9 calls: <total>/s, <n>/s from other threads, <calls per Present> per
     Present (<Presents>/s); top: <Interface>::<Method> <n>/s (<m>/Present), ...
   `counts` and `names` have `n` entries. The list holds the `top` busiest pairs
   that were called at all, busiest first, ties in name order. Per-second
   figures are rounded to whole numbers, per-Present ones to one decimal place.
   A period without a Present says "no Present" once, in place of the header's
   per-Present figures, and the pairs carry no per-Present figure. */
inline std::string
FormatCallReport(
    const uint32_t *counts, const char *const *names, size_t n, uint64_t other_thread, uint64_t presents,
    uint64_t elapsed_ns, size_t top = kReportTop
) {
  const double seconds = double(elapsed_ns ? elapsed_ns : 1) / 1e9;
  auto per_second = [seconds](uint64_t count) {
    char text[32];
    std::snprintf(text, sizeof(text), "%lld", (long long)std::llround(double(count) / seconds));
    return std::string(text);
  };
  auto per_present = [presents](uint64_t count) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", double(count) / double(presents));
    return std::string(text);
  };

  uint64_t total = 0;
  std::vector<uint32_t> used;
  for (size_t i = 0; i < n; i++) {
    total += counts[i];
    if (counts[i])
      used.push_back(uint32_t(i));
  }
  const size_t shown = std::min(top, used.size());
  std::partial_sort(used.begin(), used.begin() + shown, used.end(), [counts, names](uint32_t a, uint32_t b) {
    if (counts[a] != counts[b])
      return counts[a] > counts[b];
    return std::strcmp(names[a], names[b]) < 0;
  });

  std::string line = "d3d9 calls: " + per_second(total) + "/s, " + per_second(other_thread) + "/s from other threads, ";
  if (presents)
    line += per_present(total) + " per Present (" + per_second(presents) + "/s)";
  else
    line += "no Present";
  line += "; top:";
  for (size_t k = 0; k < shown; k++) {
    const uint32_t id = used[k];
    line += k ? ", " : " ";
    line += names[id];
    line += " " + per_second(counts[id]) + "/s";
    if (presents)
      line += " (" + per_present(counts[id]) + "/Present)";
  }
  if (!shown)
    line += " none";
  return line;
}

/* The counters of the current period. count() is the hot path: a plain,
   non-atomic 32-bit add. Counts lost to threads racing on the same counter are
   accepted (spec 07 clarification 3). Only the period's start is atomic: the
   Present that ends a period and the export that starts the first one can come
   from different threads. */
class CallTable {
public:
  void
  count(unsigned id) {
    counts_[id] += 1;
  }

  void
  countOtherThread() {
    other_thread_ += 1;
  }

  void
  countPresent() {
    presents_ += 1;
  }

  /* The first counted call starts the first period; later calls change
     nothing. */
  void
  startIfIdle(uint64_t now_ns) {
    if (start_ns_.load(std::memory_order_relaxed) != 0)
      return;
    uint64_t idle = 0;
    start_ns_.compare_exchange_strong(idle, now_ns ? now_ns : 1, std::memory_order_relaxed);
  }

  /* A Present at `now_ns` reports when the period is at least
     kReportPeriodNs old. */
  bool
  reportDue(uint64_t now_ns) const {
    const uint64_t start = start_ns_.load(std::memory_order_relaxed);
    return start != 0 && now_ns >= start && now_ns - start >= kReportPeriodNs;
  }

  /* Ends the period at `now_ns`: returns its report line, resets every counter
     and starts the next period at `now_ns`. One thread at a time (the caller
     serialises reports). */
  std::string
  takeReport(uint64_t now_ns, size_t top = kReportTop) {
    uint32_t counts[kD9CallCount];
    std::memcpy(counts, counts_, sizeof(counts));
    std::memset(counts_, 0, sizeof(counts_));
    const uint32_t other_thread = other_thread_;
    const uint32_t presents = presents_;
    other_thread_ = 0;
    presents_ = 0;
    const uint64_t start = start_ns_.load(std::memory_order_relaxed);
    start_ns_.store(now_ns ? now_ns : 1, std::memory_order_relaxed);
    const uint64_t elapsed = now_ns > start ? now_ns - start : 0;
    return FormatCallReport(counts, kD9CallNames, kD9CallCount, other_thread, presents, elapsed, top);
  }

  uint32_t
  countOf(unsigned id) const {
    return counts_[id];
  }

  uint32_t
  otherThread() const {
    return other_thread_;
  }

  uint32_t
  presents() const {
    return presents_;
  }

  uint64_t
  periodStart() const {
    return start_ns_.load(std::memory_order_relaxed);
  }

private:
  uint32_t counts_[kD9CallCount] = {};
  uint32_t other_thread_ = 0;
  uint32_t presents_ = 0;
  std::atomic<uint64_t> start_ns_{0};
};

} // namespace dxmt::d9cs
