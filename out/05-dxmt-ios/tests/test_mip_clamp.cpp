// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 Part I: mip-clamp policy tests (spec 05 D5) for the pure functions in
// dxmt/src/dxmt/dxmt_mip_clamp.hpp. No Windows or Metal types.
//
//   c++ -std=c++20 -I../dxmt/src/dxmt test_mip_clamp.cpp -o test_mip_clamp

#include "test_common.hpp"
#include "dxmt_mip_clamp.hpp"

using namespace dxmt::mipclamp;

namespace {

constexpr uint64_t MB = 1024 * 1024;
constexpr uint64_t kThreshold = 1536 * MB; // d3d11.mipClampAutoMB default

/* ---- Bias choice ------------------------------------------------------------ */

void
TestChooseBias() {
  CHECK_EQ(ChooseBias(1024, 1024, 11, 0), 0);
  CHECK_EQ(ChooseBias(1024, 1024, 11, 1), 1);
  CHECK_EQ(ChooseBias(1024, 512, 11, 3), 3); // 128 x 64
  // At least one level left: 64 x 64 with 3 levels allows bias 2 at most (16 x 16).
  CHECK_EQ(ChooseBias(64, 64, 3, 5), 2);
  // Physical top level must be a multiple of 4: 1000 >> 2 = 250 isn't, 1000 >> 1 = 500 is.
  CHECK_EQ(ChooseBias(1000, 1000, 10, 2), 1);
  // 12 >> 1 = 6: not a multiple of 4, no clamp.
  CHECK_EQ(ChooseBias(12, 12, 2, 1), 0);
  // At least 4 x 4: 16 x 8 >> 2 = 4 x 2 is too small, >> 1 = 8 x 4 is fine.
  CHECK_EQ(ChooseBias(16, 8, 5, 2), 1);
  // Non-square: 4096 x 16 >> 3 = 512 x 2 no, >> 2 = 1024 x 4 yes.
  CHECK_EQ(ChooseBias(4096, 16, 13, 3), 2);
  // One level (nothing to drop) or none.
  CHECK_EQ(ChooseBias(1024, 1024, 1, 1), 0);
  CHECK_EQ(ChooseBias(1024, 1024, 0, 1), 0);
}

void
TestIsAutoCandidate() {
  CHECK(IsAutoCandidate(1024, 1));
  CHECK(IsAutoCandidate(1, 1024));
  CHECK(IsAutoCandidate(4096, 4096));
  CHECK(!IsAutoCandidate(1023, 1023));
  CHECK(!IsAutoCandidate(512, 512));
}

/* ---- Requery rule ------------------------------------------------------------ */

void
TestShouldRequery() {
  const uint64_t plenty = 4 * kThreshold;
  // First candidate, or no reading yet.
  CHECK(ShouldRequery(0, false, 0, kThreshold));
  CHECK(ShouldRequery(0, true, plenty, kThreshold));
  CHECK(ShouldRequery(7, false, plenty, kThreshold));
  // Every 16th.
  CHECK(!ShouldRequery(1, true, plenty, kThreshold));
  CHECK(!ShouldRequery(15, true, plenty, kThreshold));
  CHECK(ShouldRequery(16, true, plenty, kThreshold));
  CHECK(!ShouldRequery(17, true, plenty, kThreshold));
  CHECK(ShouldRequery(32, true, plenty, kThreshold));
  // Every one while the last reading was below twice the threshold.
  CHECK(ShouldRequery(5, true, 2 * kThreshold - 1, kThreshold));
  CHECK(ShouldRequery(5, true, kThreshold / 2, kThreshold));
  CHECK(!ShouldRequery(5, true, 2 * kThreshold, kThreshold)); // "below", not "at"
}

/* ---- Pressure rule ------------------------------------------------------------ */

void
TestAutoBias() {
  CHECK_EQ(AutoBias(0, kThreshold), 1);
  CHECK_EQ(AutoBias(kThreshold - 1, kThreshold), 1);
  CHECK_EQ(AutoBias(kThreshold, kThreshold), 0);
  CHECK_EQ(AutoBias(10 * kThreshold, kThreshold), 0);
}

/* A headroom query that returns a scripted reading and counts its calls. */
struct FakeQuery {
  uint64_t headroom = 0;
  bool available = true;
  int calls = 0;

  bool
  operator()(uint64_t &out) {
    calls++;
    if (!available)
      return false;
    out = headroom;
    return true;
  }
};

void
TestRequestedBiasExplicitWins() {
  AutoState state;
  FakeQuery query{100 * MB};
  CHECK_EQ(RequestedBias(2, true, kThreshold, 2048, 2048, state, query), 2);
  CHECK_EQ(query.calls, 0);
  CHECK_EQ(state.candidates, 0);
}

void
TestRequestedBiasDisabledOrNotCandidate() {
  AutoState state;
  FakeQuery query{100 * MB};
  CHECK_EQ(RequestedBias(0, false, kThreshold, 2048, 2048, state, query), 0);
  CHECK_EQ(RequestedBias(0, true, kThreshold, 512, 512, state, query), 0);
  CHECK_EQ(query.calls, 0);
  CHECK_EQ(state.candidates, 0);
}

void
TestRequestedBiasPlentyOfHeadroom() {
  // 4000 MB of headroom (above twice the threshold): queried on candidate 0 and
  // 16 only, never clamped.
  AutoState state;
  FakeQuery query{4000 * MB};
  for (int i = 0; i < 17; i++)
    CHECK_EQ(RequestedBias(0, true, kThreshold, 2048, 1024, state, query), 0);
  CHECK_EQ(query.calls, 2);
  CHECK_EQ(state.candidates, 17);
}

void
TestRequestedBiasLowHeadroom() {
  // 1000 MB (below the threshold): clamped by 1, queried for every candidate.
  AutoState state;
  FakeQuery query{1000 * MB};
  for (int i = 0; i < 5; i++)
    CHECK_EQ(RequestedBias(0, true, kThreshold, 1024, 1024, state, query), 1);
  CHECK_EQ(query.calls, 5);
}

void
TestRequestedBiasNearThreshold() {
  // 2000 MB: above the threshold (no clamp) but below twice it (requery every time).
  AutoState state;
  FakeQuery query{2000 * MB};
  for (int i = 0; i < 4; i++)
    CHECK_EQ(RequestedBias(0, true, kThreshold, 1024, 1024, state, query), 0);
  CHECK_EQ(query.calls, 4);

  // Headroom drops: the next candidate sees it.
  query.headroom = 1200 * MB;
  CHECK_EQ(RequestedBias(0, true, kThreshold, 1024, 1024, state, query), 1);
}

void
TestRequestedBiasStaleReadingUntil16th() {
  // Plenty at candidate 0; the reading isn't refreshed until candidate 16.
  AutoState state;
  FakeQuery query{4000 * MB};
  CHECK_EQ(RequestedBias(0, true, kThreshold, 1024, 1024, state, query), 0);
  query.headroom = 500 * MB;
  for (int i = 1; i < 16; i++)
    CHECK_EQ(RequestedBias(0, true, kThreshold, 1024, 1024, state, query), 0);
  CHECK_EQ(RequestedBias(0, true, kThreshold, 1024, 1024, state, query), 1); // candidate 16
  CHECK_EQ(RequestedBias(0, true, kThreshold, 1024, 1024, state, query), 1); // low: every one
  CHECK_EQ(query.calls, 3);
}

void
TestRequestedBiasQueryUnavailable() {
  // No query: never clamp automatically, and don't ask again.
  AutoState state;
  FakeQuery query;
  query.available = false;
  CHECK_EQ(RequestedBias(0, true, kThreshold, 4096, 4096, state, query), 0);
  CHECK(state.query_unavailable);
  for (int i = 0; i < 20; i++)
    CHECK_EQ(RequestedBias(0, true, kThreshold, 4096, 4096, state, query), 0);
  CHECK_EQ(query.calls, 1);
  // An explicit bias still applies.
  CHECK_EQ(RequestedBias(1, true, kThreshold, 4096, 4096, state, query), 1);
}

/* ---- View ranges and lost levels ------------------------------------------------ */

void
TestTranslateViewRange() {
  uint32_t first = 99, count = 99;
  // Whole logical chain (11 levels) on a texture clamped by 1 (10 physical levels).
  TranslateViewRange(1, 10, 0, 11, first, count);
  CHECK_EQ(first, 0);
  CHECK_EQ(count, 10);
  // Logical [3, 5) -> physical [2, 4).
  TranslateViewRange(1, 10, 3, 2, first, count);
  CHECK_EQ(first, 2);
  CHECK_EQ(count, 2);
  // Only dropped levels asked for: clamp to the top physical level, never empty.
  TranslateViewRange(2, 9, 0, 1, first, count);
  CHECK_EQ(first, 0);
  CHECK_EQ(count, 1);
  // Past the end: the last physical level.
  TranslateViewRange(0, 4, 6, 1, first, count);
  CHECK_EQ(first, 3);
  CHECK_EQ(count, 1);
  // No clamp: unchanged.
  TranslateViewRange(0, 5, 1, 2, first, count);
  CHECK_EQ(first, 1);
  CHECK_EQ(count, 2);
}

void
TestMinLodForLostLevels() {
  CHECK(MinLodForLostLevels(0, 0, 5) == 0.0f);
  CHECK(MinLodForLostLevels(0b1, 0, 5) == 1.0f);   // level 0 lost -> start at 1
  CHECK(MinLodForLostLevels(0b11, 0, 5) == 2.0f);  // 0 and 1 lost -> start at 2
  CHECK(MinLodForLostLevels(0b100, 0, 5) == 0.0f); // first level fine
  CHECK(MinLodForLostLevels(0b1100, 2, 4) == 2.0f); // view from 2: levels 2, 3 lost
  CHECK(MinLodForLostLevels(0b111, 0, 3) == 2.0f); // everything lost: last level
}

/* ---- Refill filter ---------------------------------------------------------------- */

void
TestBoxFilterUnsigned() {
  // 4 x 2 -> 2 x 1: (0+1+4+5+2)/4 = 3, (2+3+6+7+2)/4 = 5.
  const uint8_t src[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint8_t dst[2];
  BoxFilter2x2(src, 0, 4, 2, dst, 0, 1, false);
  CHECK_EQ(dst[0], 3);
  CHECK_EQ(dst[1], 5);

  // Rounded mean: 0.25 -> 0, 0.5 -> 1, 0.75 -> 1.
  const uint8_t quarter[4] = {0, 0, 0, 1}, half[4] = {0, 0, 1, 1}, three[4] = {1, 1, 1, 0};
  BoxFilter2x2(quarter, 0, 2, 2, dst, 0, 1, false);
  CHECK_EQ(dst[0], 0);
  BoxFilter2x2(half, 0, 2, 2, dst, 0, 1, false);
  CHECK_EQ(dst[0], 1);
  BoxFilter2x2(three, 0, 2, 2, dst, 0, 1, false);
  CHECK_EQ(dst[0], 1);
}

void
TestBoxFilterSigned() {
  // Mean truncated toward zero.
  const uint8_t a[4] = {0xFF, 0xFF, 0xFF, 0x00}; // -1,-1,-1,0: -0.75 -> 0
  const uint8_t b[4] = {0xFD, 0xFD, 0xFD, 0xFE}; // -3,-3,-3,-2: -2.75 -> -2
  const uint8_t c[4] = {0x7F, 0x7F, 0x7F, 0x7F}; // 127
  const uint8_t d[4] = {0x81, 0x81, 0x81, 0x81}; // -127
  const uint8_t e[4] = {3, 3, 3, 2};             // 2.75 -> 2
  uint8_t dst[1];
  BoxFilter2x2(a, 0, 2, 2, dst, 0, 1, true);
  CHECK_EQ(int8_t(dst[0]), 0);
  BoxFilter2x2(b, 0, 2, 2, dst, 0, 1, true);
  CHECK_EQ(int8_t(dst[0]), -2);
  BoxFilter2x2(c, 0, 2, 2, dst, 0, 1, true);
  CHECK_EQ(int8_t(dst[0]), 127);
  BoxFilter2x2(d, 0, 2, 2, dst, 0, 1, true);
  CHECK_EQ(int8_t(dst[0]), -127);
  BoxFilter2x2(e, 0, 2, 2, dst, 0, 1, true);
  CHECK_EQ(int8_t(dst[0]), 2);
}

void
TestBoxFilterEdgesAndChannels() {
  // 1 x 5 column -> 1 x 2: the missing column repeats; row 4 is dropped (5 / 2 = 2).
  //   (10+10+20+20+2)/4 = 15, (30+30+40+40+2)/4 = 35.
  const uint8_t column[5] = {10, 20, 30, 40, 50};
  uint8_t dst[2];
  BoxFilter2x2(column, 1, 1, 5, dst, 1, 1, false);
  CHECK_EQ(dst[0], 15);
  CHECK_EQ(dst[1], 35);

  // 1 x 1 -> 1 x 1: unchanged.
  const uint8_t one[1] = {77};
  BoxFilter2x2(one, 0, 1, 1, dst, 0, 1, false);
  CHECK_EQ(dst[0], 77);

  // 2 x 2 RGBA with a padded source pitch (12 bytes) -> one texel per channel.
  const uint8_t rgba[24] = {
      0,  100, 200, 255, 4,  104, 204, 255, 0xEE, 0xEE, 0xEE, 0xEE, //
      8,  108, 208, 251, 12, 112, 212, 251, 0xEE, 0xEE, 0xEE, 0xEE, //
  };
  uint8_t out[4];
  BoxFilter2x2(rgba, 12, 2, 2, out, 0, 4, false);
  CHECK_EQ(out[0], 6);   // (0+4+8+12+2)/4
  CHECK_EQ(out[1], 106); // (100+104+108+112+2)/4
  CHECK_EQ(out[2], 206);
  CHECK_EQ(out[3], 253); // (255+255+251+251+2)/4 = 253.5 -> 253

  // RG8 3 x 3 -> 1 x 1 uses texels (0,0), (1,0), (0,1), (1,1) only.
  const uint8_t rg[18] = {
      10, 1, 20, 2, 99, 99, //
      30, 3, 40, 4, 99, 99, //
      99, 99, 99, 99, 99, 99,
  };
  uint8_t out2[2];
  BoxFilter2x2(rg, 0, 3, 3, out2, 0, 2, false);
  CHECK_EQ(out2[0], 25); // (10+20+30+40+2)/4 = 25.5 -> 25
  CHECK_EQ(out2[1], 3);  // (1+2+3+4+2)/4 = 3
}

} // namespace

int
main() {
  TestChooseBias();
  TestIsAutoCandidate();
  TestShouldRequery();
  TestAutoBias();
  TestRequestedBiasExplicitWins();
  TestRequestedBiasDisabledOrNotCandidate();
  TestRequestedBiasPlentyOfHeadroom();
  TestRequestedBiasLowHeadroom();
  TestRequestedBiasNearThreshold();
  TestRequestedBiasStaleReadingUntil16th();
  TestRequestedBiasQueryUnavailable();
  TestTranslateViewRange();
  TestMinLodForLostLevels();
  TestBoxFilterUnsigned();
  TestBoxFilterSigned();
  TestBoxFilterEdgesAndChannels();
  return TestSummary("test_mip_clamp");
}
