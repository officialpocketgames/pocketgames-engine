// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 12: host test of when the shader cache's lookups are logged
// (header-only dxmt/src/dxmt/dxmt_shader_cache_log.hpp), with a clock the test sets: the first
// hit and the first conversion are logged at once; a second change within 10 s is silent; after
// 10 s a summary is logged if a counter changed; no change, no line; the summary text; the line
// at DLL_PROCESS_DETACH only when it differs from the last summary.
//
//   c++ -std=c++20 -I../dxmt/src/dxmt test_shader_cache_log.cpp

#include "test_common.hpp"

#include "dxmt_shader_cache_log.hpp"

#include <string>

namespace {

using dxmt::FormatShaderCacheLine;
using dxmt::ShaderCacheCounts;
using dxmt::ShaderCacheLine;
using dxmt::ShaderCacheLogRule;

/* A process's counters and its rule, as dxmt_shader_cache.cpp drives them. */
struct Process {
  ShaderCacheLogRule rule;
  ShaderCacheCounts counts;

  ShaderCacheLine
  lookup(bool hit, uint64_t now_ms) {
    (hit ? counts.hits : counts.converted)++;
    return rule.onLookup(hit, counts, now_ms);
  }

  void
  store() {
    counts.stored++;
  }
};

void
TestFirstLinesAtOnce() {
  Process p;
  CHECK(p.lookup(false, 1000) == ShaderCacheLine::FirstConversion);
  CHECK(p.lookup(false, 1100) == ShaderCacheLine::None); // within 10 s: silent
  // the first hit is logged at once, also inside the 10 s
  CHECK(p.lookup(true, 1200) == ShaderCacheLine::FirstHit);
  CHECK(p.lookup(true, 1300) == ShaderCacheLine::None);

  Process q; // a warm start: the first hit comes first
  CHECK(q.lookup(true, 0) == ShaderCacheLine::FirstHit);
  CHECK(q.lookup(true, 10) == ShaderCacheLine::None);
}

void
TestSummaryEveryTenSeconds() {
  Process p;
  CHECK(p.lookup(false, 5000) == ShaderCacheLine::FirstConversion); // the 10 s start here
  p.store();
  CHECK(p.lookup(false, 9000) == ShaderCacheLine::None);
  CHECK(p.lookup(false, 14999) == ShaderCacheLine::None); // 9.999 s after the first line
  CHECK(p.lookup(false, 15000) == ShaderCacheLine::Summary);
  CHECK_EQ(p.counts.converted, 4);
  // the next summary 10 s after this one
  CHECK(p.lookup(true, 15001) == ShaderCacheLine::FirstHit);
  CHECK(p.lookup(true, 20000) == ShaderCacheLine::None);
  CHECK(p.lookup(true, 25000) == ShaderCacheLine::Summary);
  // a long quiet stretch: the next lookup after it is summarised at once
  CHECK(p.lookup(true, 600000) == ShaderCacheLine::Summary);
}

void
TestNoChangeNoLine() {
  Process p;
  CHECK(p.lookup(true, 0) == ShaderCacheLine::FirstHit);
  CHECK(p.lookup(true, 10000) == ShaderCacheLine::Summary);
  // nothing counted since that summary: the rule stays silent however late it is asked
  CHECK(p.rule.check(p.counts, 30000) == ShaderCacheLine::None);
  CHECK(p.rule.check(p.counts, 90000) == ShaderCacheLine::None);
  // a store alone is logged with the next summary, which only a lookup can bring
  p.store();
  CHECK(p.rule.check(p.counts, 100000) == ShaderCacheLine::Summary);
}

void
TestDetach() {
  {
    Process p; // nothing looked up: no line
    CHECK(!p.rule.onDetach(p.counts));
  }
  {
    Process p;
    CHECK(p.lookup(false, 0) == ShaderCacheLine::FirstConversion);
    p.store();
    CHECK(p.rule.onDetach(p.counts)); // never summarised: due
    CHECK(!p.rule.onDetach(p.counts)); // logged once
  }
  {
    Process p;
    CHECK(p.lookup(true, 0) == ShaderCacheLine::FirstHit);
    CHECK(p.lookup(true, 10000) == ShaderCacheLine::Summary);
    CHECK(!p.rule.onDetach(p.counts)); // the same as the last summary
    p.store();
    CHECK(p.rule.onDetach(p.counts)); // a store after it: differs
  }
}

void
TestText() {
  ShaderCacheCounts counts{12, 3, 4};
  CHECK(FormatShaderCacheLine(ShaderCacheLine::FirstHit, counts) == "dxmt: shader cache: first hit");
  CHECK(
      FormatShaderCacheLine(ShaderCacheLine::FirstConversion, counts) ==
      "dxmt: shader cache: first conversion (not in the cache)"
  );
  CHECK(
      FormatShaderCacheLine(ShaderCacheLine::Summary, counts) ==
      "dxmt: shader cache: 12 hits, 3 converted, 4 stored this run"
  );
  CHECK(FormatShaderCacheLine(ShaderCacheLine::None, counts).empty());
}

} // namespace

int
main() {
  TestFirstLinesAtOnce();
  TestSummaryEveryTenSeconds();
  TestNoChangeNoLine();
  TestDetach();
  TestText();
  return TestSummary("test_shader_cache_log");
}
