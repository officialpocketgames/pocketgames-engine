// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 3 (Part D): host tests for the Direct3D 9 call
// statistics table and its report line (dxmt/src/d3d9/d3d9_call_stats_table.hpp):
// the ranking, the per-second and per-Present figures for a fake clock and
// counts, the 10 s period, the reset, ties broken by name, fewer than 16 used
// pairs and "no Present". The call-site coverage is checked by
// check-d3d9-call-sites.sh and test_d3d9_call_list.c.
//
//   c++ -std=c++20 -I../dxmt/src/d3d9 test_d3d9_call_stats.cpp -o test_d3d9_call_stats

#include "test_common.hpp"

#include "d3d9_call_stats_table.hpp"

#include <algorithm>
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

/* What the frontend's NotePresent does, on one thread: the period starts if it
   hasn't, a due report is taken first, then the Present is counted, so the
   Present that triggers a report belongs to the next period. */
std::string
Present(CallTable &t, unsigned id, uint64_t now) {
  std::string line;
  t.startIfIdle(now);
  if (t.reportDue(now))
    line = t.takeReport(now);
  t.countPresent();
  t.count(id);
  return line;
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
  const std::string line = FormatCallReport(counts, kD9CallNames, kD9CallCount, 1500, 600, 10 * kSecond);
  CHECK_STR(
      line, "d3d9 calls: 7261/s, 150/s from other threads, 121.0 per Present (60/s); top: "
            "IDirect3DDevice9::SetRenderState 6000/s (100.0/Present), "
            "IDirect3DDevice9::DrawIndexedPrimitive 1200/s (20.0/Present), "
            "IDirect3DDevice9::Present 60/s (1.0/Present), "
            "IDirect3DVertexBuffer9::Lock 1/s (0.0/Present)"
  );

  // Rounding: per second to whole numbers, per Present to one decimal place.
  uint32_t r[kD9CallCount] = {};
  r[D9C_IDirect3DDevice9_SetTexture] = 25; // 25 over 10 s = 2.5/s -> 3; 25/3 = 8.33 -> 8.3
  r[D9C_IDirect3DDevice9_SetSamplerState] = 14; // 1.4/s -> 1; 14/3 = 4.67 -> 4.7
  CHECK_STR(
      FormatCallReport(r, kD9CallNames, kD9CallCount, 0, 3, 10 * kSecond),
      "d3d9 calls: 4/s, 0/s from other threads, 13.0 per Present (0/s); top: "
      "IDirect3DDevice9::SetTexture 3/s (8.3/Present), IDirect3DDevice9::SetSamplerState 1/s (4.7/Present)"
  );

  // A period longer than 10 s divides by its real length: 30 s.
  uint32_t l[kD9CallCount] = {};
  l[D9C_IDirect3DDevice9_Clear] = 900;
  CHECK_STR(
      FormatCallReport(l, kD9CallNames, kD9CallCount, 0, 1800, 30 * kSecond),
      "d3d9 calls: 30/s, 0/s from other threads, 0.5 per Present (60/s); top: IDirect3DDevice9::Clear 30/s "
      "(0.5/Present)"
  );
}

void
TestRankingTiesAndTop() {
  // 20 used pairs with distinct counts: exactly the 16 busiest, busiest first.
  uint32_t counts[kD9CallCount] = {};
  for (unsigned i = 0; i < 20; i++)
    counts[D9C_IDirect3DDevice9_QueryInterface + i] = 100 + i * 10;
  const std::string line = FormatCallReport(counts, kD9CallNames, kD9CallCount, 0, 10, 10 * kSecond);
  size_t listed = 0;
  for (size_t p = line.find("/Present)"); p != std::string::npos; p = line.find("/Present)", p + 1))
    listed++;
  CHECK_EQ(listed, kReportTop);
  // The busiest (index 19, 290 calls: 29/s) comes first, the 16th is index 4.
  const std::string first = std::string(kD9CallNames[D9C_IDirect3DDevice9_QueryInterface + 19]) + " 29/s (29.0/Present)";
  CHECK_EQ(line.find(first), line.find("top: ") + 5);
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
  CHECK_STR(
      FormatCallReport(t, kD9CallNames, kD9CallCount, 0, 0, 10 * kSecond),
      "d3d9 calls: 23/s, 0/s from other threads, no Present; top: IDirect3DDevice9::BeginScene 8/s, "
      "IDirect3DDevice9::DrawPrimitive 5/s, IDirect3DDevice9::SetTransform 5/s, IDirect3DSurface9::LockRect 5/s"
  );

  // Ties at the cut: of 17 pairs with the same count, the 16 first by name.
  uint32_t c[kD9CallCount] = {};
  std::vector<std::string> names;
  for (unsigned i = 0; i < 17; i++) {
    c[D9C_IDirect3D9_QueryInterface + i] = 10;
    names.push_back(kD9CallNames[D9C_IDirect3D9_QueryInterface + i]);
  }
  std::sort(names.begin(), names.end());
  const std::string cut = FormatCallReport(c, kD9CallNames, kD9CallCount, 0, 1, 10 * kSecond);
  for (unsigned i = 0; i < 16; i++)
    CHECK(cut.find(names[i] + " 1/s") != std::string::npos);
  CHECK(cut.find(names[16] + " ") == std::string::npos);

  // Nothing called at all (a period always holds the Present that started it,
  // so this only guards the format).
  uint32_t none[kD9CallCount] = {};
  CHECK_STR(
      FormatCallReport(none, kD9CallNames, kD9CallCount, 0, 0, 10 * kSecond),
      "d3d9 calls: 0/s, 0/s from other threads, no Present; top: none"
  );
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
  const std::string line = Present(t, D9C_IDirect3DDevice9_Present, t0 + 10 * kSecond);
  // 10802 SetRenderState, 2160 DrawIndexedPrimitive, 540 Present, 1 Direct3DCreate9
  // = 13503 calls over 10 s; 270 from other threads; 540 Presents.
  CHECK_STR(
      line, "d3d9 calls: 1350/s, 27/s from other threads, 25.0 per Present (54/s); top: "
            "IDirect3DDevice9::SetRenderState 1080/s (20.0/Present), "
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
      loading, "d3d9 calls: 200/s, 0/s from other threads, 2401.0 per Present (0/s); top: "
               "IDirect3DTexture9::LockRect 100/s (1200.0/Present), "
               "IDirect3DTexture9::UnlockRect 100/s (1200.0/Present), "
               "IDirect3DDevice9::Present 0/s (1.0/Present)"
  );

  // A period whose report-triggering Present was the previous period's last call
  // and nothing else: still one Present (the triggering one counts here).
  const std::string quiet = Present(t, D9C_IDirect3DDevice9Ex_PresentEx, t0 + 40 * kSecond);
  CHECK_STR(
      quiet, "d3d9 calls: 0/s, 0/s from other threads, 1.0 per Present (0/s); top: "
             "IDirect3DSwapChain9::Present 0/s (1.0/Present)"
  );

  // No Present in a whole period: takeReport called for it directly (as at a
  // Present that a racing thread's report already took).
  CallTable q;
  q.startIfIdle(5 * kSecond);
  Add(q, D9C_IDirect3DDevice9_CreateTexture, 300);
  CHECK_STR(
      q.takeReport(35 * kSecond),
      "d3d9 calls: 10/s, 0/s from other threads, no Present; top: IDirect3DDevice9::CreateTexture 10/s"
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
  TestPeriodAndReset();
  return TestSummary("test_d3d9_call_stats");
}
