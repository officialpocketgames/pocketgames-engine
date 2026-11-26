// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 90 (round 20): the [dxmt-mem] lines. One thread per process asks the unix side
// for the footprint and the memory warnings every 250 ms (WMTControl ops 8 and 10) and writes
// what ReportSchedule says is due: the status line every 10 s and at once on a memory warning
// or a newly passed whole GB of footprint; the 10 largest records and the 5 busiest creation
// sites the first time DXMT's own total passes 1, 2 and 3 GB.
#include "dxmt_mem_report.hpp"

#include "dxmt_allocation.hpp"
#include "dxmt_mem_ledger.hpp"
#include "dxmt_names.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_ios_switch.hpp"
#include "util_string.hpp"
#include "winemetal.h"
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <sstream>
#include <string>
#if defined(_WIN32) && !defined(DXMT_NATIVE)
#include <windows.h>
#else
#include <thread>
#endif

namespace dxmt::memledger {

namespace {

constexpr unsigned kTickMs = 250;

/* the ledger's waiting test: the allocation is held by unfinished command chunks only */
bool
AllocationWaiting(const void *owner) {
  return static_cast<const Allocation *>(owner)->waitingForGpu();
}

std::string
FormatName(uint32_t format) {
  std::ostringstream text;
  text << WMTPixelFormat(format);
  std::string name = text.str();
  static const char prefix[] = "WMTPixelFormat";
  if (name.compare(0, sizeof(prefix) - 1, prefix) == 0)
    name.erase(0, sizeof(prefix) - 1);
  return name;
}

/* A module and offset ("UnityPlayer.dll+0x1a2b3c") where Windows can say which module the
   address is in, the bare address otherwise. */
std::string
SiteName(uint64_t site) {
  char text[320];
#if defined(_WIN32) && !defined(DXMT_NATIVE)
  HMODULE module = nullptr;
  if (GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(uintptr_t(site)), &module
      ) &&
      module) {
    char path[260];
    DWORD length = GetModuleFileNameA(module, path, DWORD(sizeof(path)));
    if (length && length < sizeof(path)) {
      const char *base = path;
      for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/')
          base = p + 1;
      }
      snprintf(
          text, sizeof(text), "%s+0x%" PRIx64, base, site - uint64_t(reinterpret_cast<uintptr_t>(module))
      );
      return text;
    }
  }
#endif
  snprintf(text, sizeof(text), "0x%" PRIx64, site);
  return text;
}

void
SleepTick() {
#if defined(_WIN32) && !defined(DXMT_NATIVE)
  ::Sleep(kTickMs);
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
#endif
}

void
Tick(ReportSchedule &schedule) {
  uint64_t now = NowMs();
  uint64_t warnings = 0, footprint = 0;
  WMTQueryMemoryWarnings(&warnings);
  WMTQueryFootprint(&footprint);
  Ledger &ledger = Global();
  ReportDue due = schedule.tick(now, warnings, footprint, ledger.liveBytes());

  if (due.warning)
    Logger::info(str::format("[dxmt-mem] memory warning (", warnings, " so far)"));
  if (due.footprint_gb)
    Logger::info(str::format(
        "[dxmt-mem] footprint passed ", due.footprint_gb, " GB (", (footprint + (512u << 10)) >> 20, " MB)"
    ));
  if (due.status || due.total_gb) {
    Totals totals = ledger.totals(now, AllocationWaiting);
    if (due.status) {
      uint64_t graphics = 0;
      bool known = WMTQueryGraphicsFootprint(&graphics);
      Logger::info(StatusLine(totals, known ? &graphics : nullptr));
    }
    if (due.warning || due.total_gb)
      Logger::info(StorageLine(totals));
  }
  if (due.total_gb) {
    Logger::info(str::format("[dxmt-mem] DXMT's Metal memory passed ", due.total_gb, " GB; the 10 largest:"));
    std::vector<Info> largest = ledger.largest(10);
    for (unsigned i = 0; i < largest.size(); i++)
      Logger::info(LargestLine(i + 1, largest[i], FormatName));
    Logger::info(SitesLine(ledger.topSites(5, now), SiteName));
  }
}

} // namespace

void
StartReport() {
#ifndef DXMT_IOS_WOW_NATIVE
  static std::atomic<bool> started = false;
  if (started.exchange(true))
    return;
  if (!env::readSwitch("DXMT_MEM_LOG", true)) {
    Logger::info("[dxmt-mem] lines off (DXMT_MEM_LOG=0); the ledger still counts");
    return;
  }
  dxmt::thread report([] {
    env::setThreadName("dxmt-mem-report");
    ReportSchedule schedule;
    for (;;) {
      Tick(schedule);
      SleepTick();
    }
  });
  report.detach();
#endif
}

} // namespace dxmt::memledger
