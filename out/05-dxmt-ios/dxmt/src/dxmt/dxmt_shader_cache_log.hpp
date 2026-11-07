// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 12: when the shader cache's lookups are logged. The first hit and the
// first conversion (a lookup that missed) of a process are logged at once; after that at most
// one summary every 10 seconds, and only when a counter changed since the last summary, checked
// on lookups (no timer thread); one last summary at DLL_PROCESS_DETACH when it differs from the
// last one logged. The rule is a plain class so the host tests can drive it with their own clock
// (tests/test_shader_cache_log.cpp); dxmt_shader_cache.cpp keeps the counters and the lock.

#pragma once

#include <cstdint>
#include <string>

namespace dxmt {

struct ShaderCacheCounts {
  uint64_t hits = 0;      // lookups that found the shader
  uint64_t converted = 0; // lookups that missed: the shader is converted
  uint64_t stored = 0;    // entries written

  bool operator==(const ShaderCacheCounts &) const = default;
};

enum class ShaderCacheLine { None, FirstHit, FirstConversion, Summary };

constexpr uint64_t kShaderCacheSummaryIntervalMs = 10000;

class ShaderCacheLogRule {
public:
  /* A lookup was just counted into `counts` (a hit, or a miss when !hit) at `now_ms`, a
     monotonic clock. Returns the line to log for it. */
  ShaderCacheLine
  onLookup(bool hit, const ShaderCacheCounts &counts, uint64_t now_ms) {
    bool &first_done = hit ? first_hit_done_ : first_conversion_done_;
    if (!first_done) {
      first_done = true;
      if (!window_started_) {
        // the 10 s before the first summary start at the first line
        window_started_ = true;
        window_start_ms_ = now_ms;
      }
      return hit ? ShaderCacheLine::FirstHit : ShaderCacheLine::FirstConversion;
    }
    return check(counts, now_ms);
  }

  /* The summary rule alone: a summary when 10 s have passed since the last one (or since the
     first line) and a counter changed since the last summary. */
  ShaderCacheLine
  check(const ShaderCacheCounts &counts, uint64_t now_ms) {
    if (!window_started_) {
      window_started_ = true;
      window_start_ms_ = now_ms;
      return ShaderCacheLine::None;
    }
    if (now_ms < window_start_ms_ + kShaderCacheSummaryIntervalMs || counts == last_summary_)
      return ShaderCacheLine::None;
    last_summary_ = counts;
    window_start_ms_ = now_ms;
    return ShaderCacheLine::Summary;
  }

  /* DLL_PROCESS_DETACH: true when a last summary is due (the counts differ from the last
     summary logged; a process that never looked anything up logs nothing). */
  bool
  onDetach(const ShaderCacheCounts &counts) {
    if (counts == last_summary_)
      return false;
    last_summary_ = counts;
    return true;
  }

private:
  bool first_hit_done_ = false;
  bool first_conversion_done_ = false;
  bool window_started_ = false;
  uint64_t window_start_ms_ = 0;
  ShaderCacheCounts last_summary_{};
};

inline std::string
FormatShaderCacheLine(ShaderCacheLine line, const ShaderCacheCounts &counts) {
  switch (line) {
  case ShaderCacheLine::FirstHit:
    return "dxmt: shader cache: first hit";
  case ShaderCacheLine::FirstConversion:
    return "dxmt: shader cache: first conversion (not in the cache)";
  case ShaderCacheLine::Summary:
    return "dxmt: shader cache: " + std::to_string(counts.hits) + " hits, " + std::to_string(counts.converted) +
           " converted, " + std::to_string(counts.stored) + " stored this run";
  case ShaderCacheLine::None:
    break;
  }
  return {};
}

} // namespace dxmt
