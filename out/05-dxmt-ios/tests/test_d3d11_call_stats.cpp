// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 63 (DXMT_D11_CALL_STATS): the counter table and its report
// (dxmt/src/d3d11/d3d11_call_stats_table.hpp) with a fake clock: the switch's values, ranking
// with ties by name, rates, per-Present figures, the reset, fewer than 16 methods, "no Present",
// the def: mark, level 2's times, the work, pass and cut lines, and every line under the
// logger's cap with eight-digit rates. That every method is counted once is checked by
// check-d3d11-call-sites.sh and test_d3d11_call_list.c.

#include "test_common.hpp"

#include "d3d11_call_stats_table.hpp"
#include "log/log_cap.hpp"

#include <string>
#include <vector>

namespace {

using namespace dxmt;
using namespace dxmt::d11cs;

constexpr uint64_t kSecond = 1000000000ull;

bool
Contains(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

void
TestSwitch() {
  CHECK_EQ(D11StatsSettingFromValue(nullptr).level, 0u);
  CHECK_EQ(D11StatsSettingFromValue("").level, 0u);
  CHECK_EQ(D11StatsSettingFromValue("0").level, 0u);
  CHECK(!D11StatsSettingFromValue("0").ignored);
  CHECK_EQ(D11StatsSettingFromValue("1").level, 1u);
  CHECK_EQ(D11StatsSettingFromValue("2").level, 2u);
  for (const char *bad : {"3", "12", "on", "-1", "2 "}) {
    CHECK_EQ(D11StatsSettingFromValue(bad).level, 0u);
    CHECK(D11StatsSettingFromValue(bad).ignored);
  }
}

void
TestIds() {
  CHECK_EQ(unsigned(kD11ContextMethods), 149u);
  CHECK_EQ(unsigned(kD11DeviceMethods), 69u);
  CHECK_EQ(kD11CallCount, 2u * 149 + 69 + 5);
  CHECK(std::string(kD11CallNames[kD11IdImmediate + D11X_Draw]) == "Draw");
  CHECK(std::string(kD11CallNames[kD11IdDeferred + D11X_Draw]) == "def:Draw");
  CHECK(std::string(kD11CallNames[kD11IdDevice + D11D_CreateBuffer]) == "Device::CreateBuffer");
  CHECK(std::string(kD11CallNames[kD11IdOther + D11O_SwapChain_Present]) == "SwapChain::Present");
  CHECK(std::string(kD11CallNames[kD11IdOther + D11O_Child_Release]) == "Child::Release");
  CHECK_EQ(D11ContextMethodOf(kD11IdDeferred + D11X_Map), unsigned(D11X_Map));
  CHECK_EQ(D11ContextMethodOf(kD11IdDevice), unsigned(kD11ContextMethods));
}

void
TestReport() {
  D11CallTable table;
  table.startIfIdle(5 * kSecond);
  // 10 s: 1000 draws and 500 deferred draws, a tie between two methods, 100 Presents
  for (int i = 0; i < 1000; i++)
    table.count(kD11IdImmediate + D11X_Draw);
  for (int i = 0; i < 500; i++)
    table.count(kD11IdDeferred + D11X_Draw);
  for (int i = 0; i < 200; i++) {
    table.count(kD11IdImmediate + D11X_PSSetShader);
    table.count(kD11IdImmediate + D11X_IASetInputLayout);
  }
  for (int i = 0; i < 30; i++)
    table.countOtherThread();
  for (int i = 0; i < 100; i++) {
    table.count(kD11IdOther + D11O_SwapChain_Present);
    table.countPresent();
  }
  table.work().draws = 1500;
  table.work().draws_indirect = 50;
  table.work().map_discard = 400;
  table.work().discards[1][unsigned(DiscardOutcome::NewBuffer)] = 186;
  table.work().discard_new_bytes[1] = 186 * 4096;
  table.work().repeated[D11X_IASetInputLayout] = 150;
  table.work().repeated[D11X_OMSetBlendState] = 20;
  table.work().pass_starts[unsigned(PassStart::Targets)] = 300;
  D11Passes passes;
  passes.render = 5400;
  passes.compute = 9200;
  passes.cut_would[unsigned(Cut::KeepDsv)] = 300;
  passes.bytes_avoidable = 100ull * 103 * 1024 * 1024;

  CHECK(!table.reportDue(14 * kSecond));
  CHECK(table.reportDue(15 * kSecond));
  auto lines = table.takeReport(15 * kSecond, passes, 1, 1);
  // summary, two top lines (5 methods), and the five per-Present lines of level 1
  CHECK_EQ(lines.size(), 8u);
  CHECK(lines[0] == "d3d11 calls: 200/s, 50/s on deferred contexts, 3/s from other threads, 20.0 per Present (10 "
                    "Presents/s)");
  CHECK(lines[1] == "d3d11 calls, top 1-4: Draw 100/s (10.0/Present), def:Draw 50/s (5.0/Present), "
                    "IASetInputLayout 20/s (2.0/Present), PSSetShader 20/s (2.0/Present)");
  CHECK(lines[2] == "d3d11 calls, top 5-5: SwapChain::Present 10/s (1.0/Present)");
  CHECK(lines[3] == "d3d11 work per Present: 15.0 draws (0.5 indirect), 0.0 dispatches, maps 4.0 discard / 0.0 "
                    "no-overwrite / 0.0 other, 0.0 updates, 0.0 copies, 0.0 clears, 0.0 command lists");
  CHECK(lines[4] == "d3d11 discards per Present: 0.0 next slice, 0.0 pooled, 0.0 new buffers (0.0 KB); deferred "
                    "0.0, 0.0, 1.9 (7.4 KB)");
  CHECK(lines[5] == "d3d11 state per Present: 1.7 repeated calls (IASetInputLayout 1.5, OMSetBlendState 0.2), "
                    "tables 0.0 constant-buffer, 0.0 shader-resource, 0.0 vertex-buffer, 0.0 pipeline look-ups");
  CHECK(lines[6] == "d3d11 passes per Present: 54.0 render, 92.0 compute, 0.0 blit, 0.0 clear; folded 0.0 clears; "
                    "merged 0.0 render, 0.0 compute, 0.0 blit; started after: targets 3.0, clear 0.0, copy 0.0, "
                    "dispatch 0.0, commit 0.0, list 0.0, other 0.0");
  CHECK(lines[7] == "d3d11 cuts per Present: keep-dsv 0.0/3.0, ro-store 0.0/0.0, discard 0.0/0.0, overwrite-load "
                    "0.0/0.0; 0.0 MB avoided, 103.0 MB more with the switches on");

  // the table is reset and the next period starts at 15 s
  CHECK_EQ(table.countOf(kD11IdImmediate + D11X_Draw), 0u);
  CHECK_EQ(table.presents(), 0u);
  CHECK(!table.reportDue(24 * kSecond));
  CHECK(table.reportDue(25 * kSecond));

  // no Present in the period, at level 2
  table.count(kD11IdDevice + D11D_CreateBuffer);
  table.addTicks(kD11IdDevice + D11D_CreateBuffer, 48, false);
  auto quiet = table.takeReport(25 * kSecond, D11Passes{}, 2, 24000000);
  CHECK(quiet[0] == "d3d11 calls: 0/s, 0/s on deferred contexts, 0/s from other threads, no Present");
  CHECK(quiet[1] == "d3d11 calls, top 1-1: Device::CreateBuffer 0/s 2000 ns");
  CHECK(quiet.back() == "d3d11 time per Present: no Present");
  CHECK(quiet[2] == "d3d11 work per Present: no Present");
}

void
TestCutsAndTime() {
  std::vector<uint32_t> counts(kD11CallCount, 0);
  std::vector<uint64_t> ticks(kD11CallCount, 0);
  counts[kD11IdImmediate + D11X_Draw] = 24000;
  ticks[kD11IdImmediate + D11X_Draw] = 24000ull * 12; // 0.5 us each at 24 MHz
  D11Passes passes;
  passes.cut_applied[unsigned(Cut::KeepDsv)] = 200;
  passes.cut_would[unsigned(Cut::RoStore)] = 800;
  passes.bytes_avoided = 200ull * 1024 * 1024;
  passes.bytes_avoidable = 59ull * 100 * 1024 * 1024;
  passes.encode_ns = 100ull * 8 * 1000000;
  D11Period p;
  p.counts = counts.data();
  p.ticks = ticks.data();
  p.tick_hz = 24000000;
  p.presents = 100;
  p.elapsed_ns = 10 * kSecond;
  p.passes = &passes;
  p.present_thread_ticks = 100ull * 24000 * 51; // 51 ms a Present
  p.other_thread_ticks = 0;
  p.level = 2;
  auto lines = FormatD11Report(p);
  CHECK(lines[1] == "d3d11 calls, top 1-1: Draw 2400/s (240.0/Present) 500 ns");
  CHECK(lines[lines.size() - 2] == "d3d11 cuts per Present: keep-dsv 2.0/0.0, ro-store 0.0/8.0, discard 0.0/0.0, "
                                   "overwrite-load 0.0/0.0; 2.0 MB avoided, 59.0 MB more with the switches on");
  CHECK(lines.back() == "d3d11 time per Present: 51.0 ms in calls on the presenting thread, 0.0 ms on other "
                        "threads, 8.0 ms encoding");
}

void
TestCap() {
  // every method called, eight-digit rates: every line fits the logger's cap with its prefix
  std::vector<uint32_t> counts(kD11CallCount, 0);
  std::vector<uint64_t> ticks(kD11CallCount, 0);
  for (unsigned i = 0; i < kD11CallCount; i++) {
    counts[i] = 4000000000u - i;
    ticks[i] = uint64_t(counts[i]) * 1000;
  }
  D11Work work;
  work.draws = 4000000000u;
  for (auto &r : work.repeated)
    r = 4000000000u;
  for (auto &s : work.pass_starts)
    s = 4000000000u;
  D11Passes passes;
  passes.render = passes.compute = passes.blit = passes.clear = 4000000000ull;
  for (auto &c : passes.cut_applied)
    c = 4000000000ull;
  for (auto &c : passes.cut_would)
    c = 4000000000ull;
  passes.bytes_avoided = passes.bytes_avoidable = ~0ull >> 8;
  D11Period p;
  p.counts = counts.data();
  p.ticks = ticks.data();
  p.tick_hz = 24000000;
  p.presents = 1;
  p.elapsed_ns = 100000000; // 0.1 s: rates of 10 digits
  p.work = &work;
  p.passes = &passes;
  p.present_thread_ticks = ~0ull >> 8;
  p.level = 2;
  auto lines = FormatD11Report(p);
  CHECK_EQ(lines.size(), 1u + 4 + 6);
  for (auto &line : lines)
    CHECK(FormatLogLine("info:  ", line).size() <= kLogMessageMaxBytes);
  // fewer than 16 methods: as many as were called
  std::vector<uint32_t> few(kD11CallCount, 0);
  few[kD11IdImmediate + D11X_Map] = 3;
  few[kD11IdImmediate + D11X_Unmap] = 3;
  D11Period q;
  q.counts = few.data();
  q.presents = 1;
  q.elapsed_ns = kSecond;
  auto short_report = FormatD11Report(q);
  CHECK(short_report[1] == "d3d11 calls, top 1-2: Map 3/s (3.0/Present), Unmap 3/s (3.0/Present)");
  CHECK(Contains(short_report[0], "6.0 per Present (1 Presents/s)"));
}

} // namespace

int
main() {
  TestSwitch();
  TestIds();
  TestReport();
  TestCutsAndTime();
  TestCap();
  return TestSummary("test_d3d11_call_stats");
}
