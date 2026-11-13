// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 63: the call-statistics switch, the one counter table, the period clock
// and the report (see d3d11_call_stats.hpp).

#include "d3d11_call_stats.hpp"

#include "dxmt_pass_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace dxmt::d11cs {

namespace {

/* Set while the DLL loads; warned about when a device is made (the logger may not be ready
   that early). */
bool g_setting_ignored = false;

unsigned
ReadLevel() {
  const std::string value = env::getEnvVar("DXMT_D11_CALL_STATS");
  auto setting = D11StatsSettingFromValue(value.c_str());
  g_setting_ignored = setting.ignored;
  if (setting.level)
    g_encoder_pass_stats_on.store(true, std::memory_order_relaxed);
  return setting.level;
}

uint64_t
NowNs() {
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return ns > 0 ? uint64_t(ns) : 1;
}

/* A small number for the calling thread, given on its first counted call */
uint32_t
ThreadId() {
  static std::atomic<uint32_t> next{1};
  static thread_local const uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
  return id;
}

D11CallTable g_table;
std::atomic<uint32_t> g_device_thread{0};
std::atomic<uint32_t> g_present_thread{0};
std::atomic<bool> g_reporting{false};

/* The encoding thread's counters since the last report, reset. */
D11Passes
TakePasses() {
  auto &s = g_encoder_pass_stats;
  D11Passes passes;
  passes.render = PassStatTake(s.render);
  passes.compute = PassStatTake(s.compute);
  passes.blit = PassStatTake(s.blit);
  passes.clear = PassStatTake(s.clear);
  passes.folded_clears = PassStatTake(s.folded_clears);
  passes.merged_render = PassStatTake(s.merged_render);
  passes.merged_compute = PassStatTake(s.merged_compute);
  passes.merged_blit = PassStatTake(s.merged_blit);
  for (unsigned c = 0; c < unsigned(PassCut::Count); c++) {
    passes.cut_applied[c] = PassStatTake(s.cut_applied[c]);
    passes.cut_would[c] = PassStatTake(s.cut_would[c]);
  }
  passes.bytes_avoided = PassStatTake(s.bytes_avoided);
  passes.bytes_avoidable = PassStatTake(s.bytes_avoidable);
  passes.encode_ns = PassStatTake(s.encode_ns);
  return passes;
}

} // namespace

unsigned g_level = ReadLevel();

void
NoteCall(unsigned id) {
  g_table.count(id);
  const uint32_t device_thread = g_device_thread.load(std::memory_order_relaxed);
  if (device_thread != 0 && ThreadId() != device_thread)
    g_table.countOtherThread();
}

void
AddTicks(unsigned id, uint64_t ticks) {
  g_table.addTicks(id, ticks, ThreadId() == g_present_thread.load(std::memory_order_relaxed));
}

D11Work &
Work() {
  return g_table.work();
}

void
NotePresent(unsigned id) {
  const uint64_t now = NowNs();
  g_present_thread.store(ThreadId(), std::memory_order_relaxed);
  g_table.startIfIdle(now);
  if (g_table.reportDue(now) && !g_reporting.exchange(true, std::memory_order_acquire)) {
    std::vector<std::string> lines;
    if (g_table.reportDue(now))
      lines = g_table.takeReport(now, TakePasses(), g_level, TickHz());
    g_reporting.store(false, std::memory_order_release);
    for (const auto &line : lines)
      Logger::info(line);
  }
  g_table.countPresent();
  NoteCall(id);
}

void
NoteDeviceThread() {
  static std::atomic<bool> warned{false};
  if (g_setting_ignored && !warned.exchange(true))
    Logger::warn("dxmt: DXMT_D11_CALL_STATS ignored (not 0, 1 or 2)");
  if (g_level) {
    g_device_thread.store(ThreadId(), std::memory_order_relaxed);
    g_table.startIfIdle(NowNs());
  }
}

} // namespace dxmt::d11cs
