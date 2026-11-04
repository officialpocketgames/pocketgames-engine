// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarifications 3 and 4 (Part D): host tests for the Direct3D 9 call
// statistics table and its report lines (dxmt/src/d3d9/d3d9_call_stats_table.hpp):
// the ranking, the per-second and per-Present figures for a fake clock and
// counts, the 10 s period, the reset, ties broken by name, fewer than 16 used
// pairs and "no Present"; the summary line plus at most four pairs per line,
// and 16 pairs with eight-digit rates in five lines of at most 900 bytes. The
// call-site coverage is checked by check-d3d9-call-sites.sh and
// test_d3d9_call_list.c; the logger's cap by test_log_cap.cpp.
//
//   c++ -std=c++20 -I../dxmt/src/d3d9 test_d3d9_call_stats.cpp -o test_d3d9_call_stats

#include "test_common.hpp"

#include "d3d9_call_stats_table.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace dxmt::d9cs;

namespace {

constexpr uint64_t kSecond = 1000ull * 1000 * 1000;

#define CHECK_STR(a, b)                                                                                                \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    const std::string sa_ = (a), sb_ = (b);                                                                            \
    if (sa_ != sb_) {                                                                                                  \
      g_failures++;                                                                                                    \
      std::printf("FAIL %s:%d:\n  got  %s\n  want %s\n", __FILE__, __LINE__, sa_.c_str(), sb_.c_str());               \
    }                                                                                                                  \
  } while (0)

/* A report's lines, one per text line, to compare with an expected text. */
std::string
Lines(const std::vector<std::string> &lines) {
  std::string text;
  for (size_t i = 0; i < lines.size(); i++)
    text += (i ? "\n" : "") + lines[i];
  return text;
}

std::string
Format(const uint32_t *counts, uint64_t other_thread, uint64_t presents, uint64_t elapsed_ns) {
  return Lines(FormatCallReport(counts, kD9CallNames, kD9CallCount, other_thread, presents, elapsed_ns));
}

/* What the frontend's NotePresent does, on one thread: the period starts if it
   hasn't, a due report is taken first, then the Present is counted, so the
   Present that triggers a report belongs to the next period. */
std::string
Present(CallTable &t, unsigned id, uint64_t now) {
  std::string text;
  t.startIfIdle(now);
  if (t.reportDue(now))
    text = Lines(t.takeReport(now));
  t.countPresent();
  t.count(id);
  return text;
}

void
Add(CallTable &t, unsigned id, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    t.count(id);
}

void
TestNames() {
  // One name per pair, all different, all "<interface>::<method>".
  std::set<std::string> names;
  for (unsigned i = 0; i < kD9CallCount; i++) {
    const std::string name = kD9CallNames[i];
    CHECK(name.find("::") != std::string::npos);
    names.insert(name);
  }
  CHECK_EQ(names.size(), kD9CallCount);
  CHECK_STR(kD9CallNames[D9C_IDirect3DDevice9_SetRenderState], "IDirect3DDevice9::SetRenderState");
  CHECK_STR(kD9CallNames[D9C_IDirect3DDevice9Ex_PresentEx], "IDirect3DDevice9Ex::PresentEx");
  CHECK_STR(kD9CallNames[D9C_IDirect3DTexture9_AddRef], "IDirect3DTexture9::AddRef");
  CHECK_STR(kD9CallNames[D9C_d3d9_Direct3DCreate9], "d3d9::Direct3DCreate9");
  // 320 slots of the 15 d3d9.h vtables, IDxmtDiag9's 1, the validator's 6 and
  // the 12 exports.
  CHECK_EQ(kD9CallCount, 320 + 1 + 6 + 12);
}

void
TestReportFigures() {
  // 10 s, 600 Presents: 60/s. 60000 SetRenderState: 6000/s, 100.0 per Present.
  uint32_t counts[kD9CallCount] = {};
  counts[D9C_IDirect3DDevice9_SetRenderState] = 60000;
  counts[D9C_IDirect3DDevice9_DrawIndexedPrimitive] = 12000;
  counts[D9C_IDirect3DDevice9_Present] = 600;
  counts[D9C_IDirect3DVertexBuffer9_Lock] = 7;
  CHECK_STR(
      Format(counts, 1500, 600, 10 * kSecond),
      "d3d9 calls: 7261/s, 150/s from other threads, 121.0 per Present (60/s)\n"
      "d3d9 calls, top 1-4: IDirect3DDevice9::SetRenderState 6000/s (100.0/Present), "
      "IDirect3DDevice9::DrawIndexedPrimitive 1200/s (20.0/Present), "
      "IDirect3DDevice9::Present 60/s (1.0/Present), "
      "IDirect3DVertexBuffer9::Lock 1/s (0.0/Present)"
  );

  // Rounding: per second to whole numbers, per Present to one decimal place.
  uint32_t r[kD9CallCount] = {};
  r[D9C_IDirect3DDevice9_SetTexture] = 25; // 25 over 10 s = 2.5/s -> 3; 25/3 = 8.33 -> 8.3
  r[D9C_IDirect3DDevice9_SetSamplerState] = 14; // 1.4/s -> 1; 14/3 = 4.67 -> 4.7
  CHECK_STR(
      Format(r, 0, 3, 10 * kSecond),
      "d3d9 calls: 4/s, 0/s from other threads, 13.0 per Present (0/s)\n"
      "d3d9 calls, top 1-2: IDirect3DDevice9::SetTexture 3/s (8.3/Present), "
      "IDirect3DDevice9::SetSamplerState 1/s (4.7/Present)"
  );

  // A period longer than 10 s divides by its real length: 30 s.
  uint32_t l[kD9CallCount] = {};
  l[D9C_IDirect3DDevice9_Clear] = 900;
  CHECK_STR(
      Format(l, 0, 1800, 30 * kSecond),
      "d3d9 calls: 30/s, 0/s from other threads, 0.5 per Present (60/s)\n"
      "d3d9 calls, top 1-1: IDirect3DDevice9::Clear 30/s (0.5/Present)"
  );
}

void
TestRankingTiesAndTop() {
  // 20 used pairs with distinct counts: exactly the 16 busiest, busiest first,
  // four per line after the summary.
  uint32_t counts[kD9CallCount] = {};
  for (unsigned i = 0; i < 20; i++)
    counts[D9C_IDirect3DDevice9_QueryInterface + i] = 100 + i * 10;
  const auto lines = FormatCallReport(counts, kD9CallNames, kD9CallCount, 0, 10, 10 * kSecond);
  CHECK_EQ(lines.size(), 5);
  const char *const heads[] = {"d3d9 calls, top 1-4: ", "d3d9 calls, top 5-8: ", "d3d9 calls, top 9-12: ",
                               "d3d9 calls, top 13-16: "};
  for (size_t k = 1; k < lines.size() && k <= 4; k++) {
    CHECK_EQ(lines[k].find(heads[k - 1]), 0);
    size_t pairs = 0;
    for (size_t p = lines[k].find("/Present)"); p != std::string::npos; p = lines[k].find("/Present)", p + 1))
      pairs++;
    CHECK_EQ(pairs, 4);
  }
  const std::string line = Lines(lines);
  // The busiest (index 19, 290 calls: 29/s) comes first, the 16th is index 4.
  const std::string first = std::string(kD9CallNames[D9C_IDirect3DDevice9_QueryInterface + 19]) + " 29/s (29.0/Present)";
  CHECK_EQ(line.find(first), line.find("top 1-4: ") + 9);
  CHECK(line.find(kD9CallNames[D9C_IDirect3DDevice9_QueryInterface + 4]) != std::string::npos);
  CHECK(line.find(kD9CallNames[D9C_IDirect3DDevice9_QueryInterface + 3]) == std::string::npos);
  // Busiest first throughout.
  size_t last = 0;
  for (int i = 19; i >= 4; i--) {
    const size_t at = line.find(std::string(kD9CallNames[D9C_IDirect3DDevice9_QueryInterface + i]) + " ");
    CHECK(at != std::string::npos && at > last);
    last = at;
  }

  // Ties: equal counts in name order, whatever the id order.
  uint32_t t[kD9CallCount] = {};
  t[D9C_IDirect3DDevice9_SetTransform] = 50;   // "...::SetTransform"
  t[D9C_IDirect3DDevice9_DrawPrimitive] = 50;  // "...::DrawPrimitive"
  t[D9C_IDirect3DSurface9_LockRect] = 50;      // "IDirect3DSurface9::LockRect"
  t[D9C_IDirect3DDevice9_BeginScene] = 80;
  t[D9C_IDirect3DDevice9_EndScene] = 10;
  CHECK_STR(
      Format(t, 0, 0, 10 * kSecond),
      "d3d9 calls: 24/s, 0/s from other threads, no Present\n"
      "d3d9 calls, top 1-4: IDirect3DDevice9::BeginScene 8/s, IDirect3DDevice9::DrawPrimitive 5/s, "
      "IDirect3DDevice9::SetTransform 5/s, IDirect3DSurface9::LockRect 5/s\n"
      "d3d9 calls, top 5-5: IDirect3DDevice9::EndScene 1/s"
  );

  // Ties at the cut: of 17 pairs with the same count, the 16 first by name.
  uint32_t c[kD9CallCount] = {};
  std::vector<std::string> names;
  for (unsigned i = 0; i < 17; i++) {
    c[D9C_IDirect3D9_QueryInterface + i] = 10;
    names.push_back(kD9CallNames[D9C_IDirect3D9_QueryInterface + i]);
  }
  std::sort(names.begin(), names.end());
  const std::string cut = Format(c, 0, 1, 10 * kSecond);
  for (unsigned i = 0; i < 16; i++)
    CHECK(cut.find(names[i] + " 1/s") != std::string::npos);
  CHECK(cut.find(names[16] + " ") == std::string::npos);

  // Nothing called at all (a period always holds the Present that started it,
  // so this only guards the format): the summary alone.
  uint32_t none[kD9CallCount] = {};
  CHECK_STR(Format(none, 0, 0, 10 * kSecond), "d3d9 calls: 0/s, 0/s from other threads, no Present");
}

void
TestLongReport() {
  // Clarification 4: 16 pairs with eight-digit rates (a heavy scene at 1 s)
  // give five lines, each far under Wine's debug buffer (at most 900 bytes,
  // the logger's cap, with room for its prefix).
  uint32_t counts[kD9CallCount] = {};
  // The longest names, so the lines are as long as they get.
  std::vector<unsigned> ids(kD9CallCount);
  for (unsigned i = 0; i < kD9CallCount; i++)
    ids[i] = i;
  std::sort(ids.begin(), ids.end(), [](unsigned a, unsigned b) {
    return std::strlen(kD9CallNames[a]) > std::strlen(kD9CallNames[b]);
  });
  for (unsigned k = 0; k < 16; k++)
    counts[ids[k]] = 99999999u - k; // eight digits per second over 1 s
  const auto lines = FormatCallReport(counts, kD9CallNames, kD9CallCount, 12345678, 1, kSecond);
  CHECK_EQ(lines.size(), 5);
  for (const auto &l : lines) {
    CHECK(l.size() + 8 <= 900); // "info:  " and the newline fit as well
    CHECK(l.size() < 600);      // well under the limit
  }
  CHECK(lines[0].find("d3d9 calls: ") == 0);
  CHECK(lines[4].find("d3d9 calls, top 13-16: ") == 0);
  // Every pair is listed once, in one of the four lines.
  for (unsigned k = 0; k < 16; k++) {
    size_t found = 0;
    for (size_t i = 1; i < lines.size(); i++)
      found += lines[i].find(std::string(kD9CallNames[ids[k]]) + " ") != std::string::npos;
    CHECK_EQ(found, 1);
  }
}

void
TestPeriodAndReset() {
  CallTable t;
  const uint64_t t0 = 1000 * kSecond;
  CHECK(!t.reportDue(t0 + 60 * kSecond)); // not started: never due
  // The first counted call (an export) starts the period; later ones don't move it.
  t.startIfIdle(t0);
  t.count(D9C_d3d9_Direct3DCreate9);
  t.startIfIdle(t0 + 5 * kSecond);
  CHECK_EQ(t.periodStart(), t0);

  // 9 s of frames: no report.
  for (int frame = 0; frame < 540; frame++) {
    Add(t, D9C_IDirect3DDevice9_SetRenderState, 20);
    Add(t, D9C_IDirect3DDevice9_DrawIndexedPrimitive, 4);
    if (frame % 2)
      t.countOtherThread();
    CHECK_STR(Present(t, D9C_IDirect3DDevice9_Present, t0 + kSecond + uint64_t(frame) * kSecond / 60), "");
  }
  CHECK(!t.reportDue(t0 + 10 * kSecond - 1));
  CHECK(t.reportDue(t0 + 10 * kSecond));
  CHECK_EQ(t.countOf(D9C_IDirect3DDevice9_SetRenderState), 540 * 20);
  CHECK_EQ(t.presents(), 540);

  // The first Present 10 s after the start reports the period and is the first
  // call of the next one.
  Add(t, D9C_IDirect3DDevice9_SetRenderState, 2);
  const std::string report = Present(t, D9C_IDirect3DDevice9_Present, t0 + 10 * kSecond);
  // 10802 SetRenderState, 2160 DrawIndexedPrimitive, 540 Present, 1 Direct3DCreate9
  // = 13503 calls over 10 s; 270 from other threads; 540 Presents.
  CHECK_STR(
      report, "d3d9 calls: 1350/s, 27/s from other threads, 25.0 per Present (54/s)\n"
              "d3d9 calls, top 1-4: IDirect3DDevice9::SetRenderState 1080/s (20.0/Present), "
              "IDirect3DDevice9::DrawIndexedPrimitive 216/s (4.0/Present), "
              "IDirect3DDevice9::Present 54/s (1.0/Present), d3d9::Direct3DCreate9 0/s (0.0/Present)"
  );

  // Reset: every counter starts again, the new period at the report.
  CHECK_EQ(t.periodStart(), t0 + 10 * kSecond);
  CHECK_EQ(t.countOf(D9C_IDirect3DDevice9_SetRenderState), 0);
  CHECK_EQ(t.countOf(D9C_IDirect3DDevice9_DrawIndexedPrimitive), 0);
  CHECK_EQ(t.countOf(D9C_d3d9_Direct3DCreate9), 0);
  CHECK_EQ(t.otherThread(), 0);
  CHECK_EQ(t.countOf(D9C_IDirect3DDevice9_Present), 1); // the triggering Present
  CHECK_EQ(t.presents(), 1);
  CHECK(!t.reportDue(t0 + 20 * kSecond - 1));

  // A loading screen: 12 s of uploads without a Present. The next Present reports
  // a period that holds only the Present that started it.
  Add(t, D9C_IDirect3DTexture9_LockRect, 1200);
  Add(t, D9C_IDirect3DTexture9_UnlockRect, 1200);
  const std::string loading = Present(t, D9C_IDirect3DSwapChain9_Present, t0 + 22 * kSecond);
  CHECK_STR(
      loading, "d3d9 calls: 200/s, 0/s from other threads, 2401.0 per Present (0/s)\n"
               "d3d9 calls, top 1-3: IDirect3DTexture9::LockRect 100/s (1200.0/Present), "
               "IDirect3DTexture9::UnlockRect 100/s (1200.0/Present), "
               "IDirect3DDevice9::Present 0/s (1.0/Present)"
  );

  // A period whose report-triggering Present was the previous period's last call
  // and nothing else: still one Present (the triggering one counts here).
  const std::string quiet = Present(t, D9C_IDirect3DDevice9Ex_PresentEx, t0 + 40 * kSecond);
  CHECK_STR(
      quiet, "d3d9 calls: 0/s, 0/s from other threads, 1.0 per Present (0/s)\n"
             "d3d9 calls, top 1-1: IDirect3DSwapChain9::Present 0/s (1.0/Present)"
  );

  // No Present in a whole period: takeReport called for it directly (as at a
  // Present that a racing thread's report already took).
  CallTable q;
  q.startIfIdle(5 * kSecond);
  Add(q, D9C_IDirect3DDevice9_CreateTexture, 300);
  CHECK_STR(
      Lines(q.takeReport(35 * kSecond)),
      "d3d9 calls: 10/s, 0/s from other threads, no Present\n"
      "d3d9 calls, top 1-1: IDirect3DDevice9::CreateTexture 10/s"
  );
  CHECK_EQ(q.countOf(D9C_IDirect3DDevice9_CreateTexture), 0);
  CHECK_EQ(q.periodStart(), 35 * kSecond);
}

} // namespace

int
main() {
  TestNames();
  TestReportFigures();
  TestRankingTiesAndTop();
  TestLongReport();
  TestPeriodAndReset();
  return TestSummary("test_d3d9_call_stats");
}
