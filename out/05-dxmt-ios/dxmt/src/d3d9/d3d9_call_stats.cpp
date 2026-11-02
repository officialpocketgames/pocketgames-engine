// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 3: the call-statistics switch, the one counter table,
// the period clock and the report line (see d3d9_call_stats.hpp).

#include "d3d9_call_stats.hpp"

#include "d3d9_ios_policy.hpp"
#include "log/log.hpp"
#include "util_env.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>

namespace dxmt::d9cs {

namespace {

bool
ReadSwitch() {
  const std::string value = env::getEnvVar("DXMT_D9_CALL_STATS");
  return d9ios::ParseSwitch(value.c_str(), false);
}

/* The monotonic clock the frontend already uses (queries, raster status). */
uint64_t
NowNs() {
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return ns > 0 ? uint64_t(ns) : 1;
}

// Constant-initialised: usable before any dynamic initialiser runs.
CallTable g_table;
std::atomic<uint32_t> g_device_thread{0};
std::atomic<bool> g_reporting{false};

} // namespace

bool g_enabled = ReadSwitch();

void
NoteCall(unsigned id) {
  g_table.count(id);
  const uint32_t device_thread = g_device_thread.load(std::memory_order_relaxed);
  if (device_thread != 0 && uint32_t(::GetCurrentThreadId()) != device_thread)
    g_table.countOtherThread();
}

void
NoteExport(unsigned id) {
  g_table.startIfIdle(NowNs());
  NoteCall(id);
}

void
NotePresent(unsigned id) {
  const uint64_t now = NowNs();
  g_table.startIfIdle(now);
  if (g_table.reportDue(now) && !g_reporting.exchange(true, std::memory_order_acquire)) {
    std::string line;
    // Checked again: another thread's Present may have reported meanwhile.
    if (g_table.reportDue(now))
      line = g_table.takeReport(now);
    g_reporting.store(false, std::memory_order_release);
    if (!line.empty())
      Logger::info(line);
  }
  g_table.countPresent();
  NoteCall(id);
}

void
NoteDeviceThread() {
  if (g_enabled)
    g_device_thread.store(uint32_t(::GetCurrentThreadId()), std::memory_order_relaxed);
}

} // namespace dxmt::d9cs
