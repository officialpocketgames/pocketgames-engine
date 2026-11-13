// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 63 (DXMT_D11_CALL_STATS): the counter table, the work counters and the
// report lines, free of Windows, Metal and device types so the host test
// (tests/test_d3d11_call_stats.cpp) builds it as it is. d3d11_call_stats.{hpp,cpp} own the one
// table the front end counts into, the switch, the clock and the log. Modelled on spec 07
// clarifications 3 and 4 (the Direct3D 9 call statistics).

#pragma once

#include "d3d11_call_list.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dxmt::d11cs {

/* ---- ids ---- */

enum D11ContextMethod : uint16_t {
#define D11CS_ID(method) D11X_##method,
  D11_CALLS_CONTEXT(D11CS_ID)
#undef D11CS_ID
      kD11ContextMethods
};

enum D11DeviceMethod : uint16_t {
#define D11CS_ID(method) D11D_##method,
  D11_CALLS_DEVICE(D11CS_ID)
#undef D11CS_ID
      kD11DeviceMethods
};

enum D11OtherMethod : uint16_t {
#define D11CS_ID(method) D11O_##method,
  D11_CALLS_OTHER(D11CS_ID)
#undef D11CS_ID
      kD11OtherMethods
};

/* One table: the immediate context's methods, the deferred contexts' (shared), the device's,
   then the others. */
constexpr unsigned kD11IdImmediate = 0;
constexpr unsigned kD11IdDeferred = kD11ContextMethods;
constexpr unsigned kD11IdDevice = 2 * kD11ContextMethods;
constexpr unsigned kD11IdOther = 2 * kD11ContextMethods + kD11DeviceMethods;
constexpr unsigned kD11CallCount = kD11IdOther + kD11OtherMethods;

/* The report's names: "Draw" for the immediate context, "def:Draw" for a deferred one,
   "Device::CreateBuffer", "SwapChain::Present", "Child::AddRef". */
inline const char *const kD11CallNames[kD11CallCount] = {
#define D11CS_NAME(method) #method,
    D11_CALLS_CONTEXT(D11CS_NAME)
#undef D11CS_NAME
#define D11CS_NAME(method) "def:" #method,
    D11_CALLS_CONTEXT(D11CS_NAME)
#undef D11CS_NAME
#define D11CS_NAME(method) "Device::" #method,
    D11_CALLS_DEVICE(D11CS_NAME)
#undef D11CS_NAME
    "SwapChain::Present",
    "SwapChain::Present1",
    "Child::QueryInterface",
    "Child::AddRef",
    "Child::Release",
};
static_assert(kD11OtherMethods == 5, "the names above follow D11_CALLS_OTHER");

/* The context method of an id in either context's range, or kD11ContextMethods. */
constexpr unsigned
D11ContextMethodOf(unsigned id) {
  return id < kD11IdDevice ? id % unsigned(kD11ContextMethods) : unsigned(kD11ContextMethods);
}

/* ---- the switch ---- */

/* Unset, empty or "0": off; "1": counts; "2": counts and times. Anything else is off, with
   `ignored` set for one warning. */
struct D11StatsSetting {
  unsigned level = 0;
  bool ignored = false;
};

inline D11StatsSetting
D11StatsSettingFromValue(const char *value) {
  D11StatsSetting setting;
  if (!value || !value[0] || (value[0] == '0' && !value[1]))
    return setting;
  if ((value[0] == '1' || value[0] == '2') && !value[1]) {
    setting.level = unsigned(value[0] - '0');
    return setting;
  }
  setting.ignored = true;
  return setting;
}

/* ---- the work counted where it happens (levels 1 and 2) ---- */

/* What ended the pass before a pass the calling thread started. */
enum class PassStart : uint8_t { Targets, Clear, Copy, Dispatch, Commit, List, Other, Count };
inline const char *const kPassStartNames[] = {"targets", "clear", "copy", "dispatch", "commit", "list", "other"};

/* A dynamic-buffer discard's outcome: the next slice of the same allocation, an allocation
   from the pool, a new Metal buffer. */
enum class DiscardOutcome : uint8_t { NextSlice, Pooled, NewBuffer, Count };

/* Items 69-72's cuts, applied or (switch off) would apply. */
enum class Cut : uint8_t { KeepDsv, RoStore, Discard, OverwriteLoad, Count };
inline const char *const kCutNames[] = {"keep-dsv", "ro-store", "discard", "overwrite-load"};

/* The API threads' counters: plain adds, races accepted, like the call counts. */
struct D11Work {
  uint32_t draws = 0, draws_indirect = 0, dispatches = 0, clears = 0, copies = 0, updates = 0, command_lists = 0;
  uint32_t map_discard = 0, map_no_overwrite = 0, map_other = 0;
  uint32_t discards[2][unsigned(DiscardOutcome::Count)] = {}; // [0] immediate, [1] deferred
  uint64_t discard_new_bytes[2] = {};
  uint32_t repeated[kD11ContextMethods] = {}; // state calls that set what was set
  uint32_t tables_cb = 0, tables_srv = 0, tables_vb = 0, pipeline_lookups = 0;
  uint32_t pass_starts[unsigned(PassStart::Count)] = {};
};

/* The encoding thread's counters (after the optimiser), read from the shared encoder
   statistics at each report. */
struct D11Passes {
  uint64_t render = 0, compute = 0, blit = 0, clear = 0;
  uint64_t folded_clears = 0, merged_render = 0, merged_compute = 0, merged_blit = 0;
  uint64_t cut_applied[unsigned(Cut::Count)] = {};
  uint64_t cut_would[unsigned(Cut::Count)] = {};
  uint64_t bytes_avoided = 0, bytes_avoidable = 0;
  uint64_t encode_ns = 0;
};

/* ---- the report ---- */

constexpr uint64_t kD11ReportPeriodNs = 10ull * 1000 * 1000 * 1000;
constexpr size_t kD11ReportTop = 16;
constexpr size_t kD11ReportPairsPerLine = 4;

/* One period's figures. `ticks` (level 2) holds each id's time in counter ticks at
   `tick_hz`; null at level 1. */
struct D11Period {
  const uint32_t *counts = nullptr;
  const uint64_t *ticks = nullptr;
  uint64_t tick_hz = 1;
  uint64_t other_thread = 0;
  uint64_t presents = 0;
  uint64_t elapsed_ns = 0;
  const D11Work *work = nullptr;
  const D11Passes *passes = nullptr;
  uint64_t present_thread_ticks = 0, other_thread_ticks = 0;
  unsigned level = 1;
};

/* The report lines (clarification 63's eight; the last at level 2 only). Per-second figures
   are whole numbers, per-Present ones have one decimal; a period without a Present says
   "no Present" for its per-Present figures. Ties among the busiest are broken by name. */
inline std::vector<std::string>
FormatD11Report(const D11Period &p, size_t top = kD11ReportTop) {
  const double seconds = double(p.elapsed_ns ? p.elapsed_ns : 1) / 1e9;
  auto per_second = [seconds](uint64_t count) {
    char text[32];
    std::snprintf(text, sizeof(text), "%lld", (long long)std::llround(double(count) / seconds));
    return std::string(text);
  };
  auto per_present = [&p](double count) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", count / double(p.presents));
    return std::string(text);
  };
  auto ms_per_present = [&p](double ns) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", ns / 1e6 / double(p.presents));
    return std::string(text);
  };
  auto ticks_ns = [&p](uint64_t ticks) { return double(ticks) * 1e9 / double(p.tick_hz ? p.tick_hz : 1); };
  static const D11Work kNoWork;
  static const D11Passes kNoPasses;
  const D11Work &w = p.work ? *p.work : kNoWork;
  const D11Passes &s = p.passes ? *p.passes : kNoPasses;

  uint64_t total = 0, deferred = 0;
  std::vector<uint32_t> used;
  for (unsigned i = 0; i < kD11CallCount; i++) {
    total += p.counts[i];
    if (i >= kD11IdDeferred && i < kD11IdDevice)
      deferred += p.counts[i];
    if (p.counts[i])
      used.push_back(i);
  }
  const size_t shown = std::min(top, used.size());
  std::partial_sort(used.begin(), used.begin() + shown, used.end(), [&p](uint32_t a, uint32_t b) {
    if (p.counts[a] != p.counts[b])
      return p.counts[a] > p.counts[b];
    return std::strcmp(kD11CallNames[a], kD11CallNames[b]) < 0;
  });

  std::vector<std::string> lines;
  std::string summary = "d3d11 calls: " + per_second(total) + "/s, " + per_second(deferred) +
                        "/s on deferred contexts, " + per_second(p.other_thread) + "/s from other threads, ";
  if (p.presents)
    summary += per_present(double(total)) + " per Present (" + per_second(p.presents) + " Presents/s)";
  else
    summary += "no Present";
  lines.push_back(std::move(summary));

  for (size_t first = 0; first < shown; first += kD11ReportPairsPerLine) {
    const size_t last = std::min(first + kD11ReportPairsPerLine, shown);
    std::string line = "d3d11 calls, top " + std::to_string(first + 1) + "-" + std::to_string(last) + ":";
    for (size_t k = first; k < last; k++) {
      const uint32_t id = used[k];
      line += k > first ? ", " : " ";
      line += kD11CallNames[id];
      line += " " + per_second(p.counts[id]) + "/s";
      if (p.presents)
        line += " (" + per_present(double(p.counts[id])) + "/Present)";
      if (p.level >= 2 && p.ticks)
        line += " " + std::to_string((long long)std::llround(ticks_ns(p.ticks[id]) / double(p.counts[id]))) + " ns";
    }
    lines.push_back(std::move(line));
  }

  if (!p.presents) {
    lines.push_back("d3d11 work per Present: no Present");
    lines.push_back("d3d11 discards per Present: no Present");
    lines.push_back("d3d11 state per Present: no Present");
    lines.push_back("d3d11 passes per Present: no Present");
    lines.push_back("d3d11 cuts per Present: no Present");
    if (p.level >= 2)
      lines.push_back("d3d11 time per Present: no Present");
    return lines;
  }

  auto pp = [&](uint64_t n) { return per_present(double(n)); };
  lines.push_back("d3d11 work per Present: " + pp(w.draws) + " draws (" + pp(w.draws_indirect) + " indirect), " +
                  pp(w.dispatches) + " dispatches, maps " + pp(w.map_discard) + " discard / " +
                  pp(w.map_no_overwrite) + " no-overwrite / " + pp(w.map_other) + " other, " + pp(w.updates) +
                  " updates, " + pp(w.copies) + " copies, " + pp(w.clears) + " clears, " + pp(w.command_lists) +
                  " command lists");

  auto kb = [&](uint64_t bytes) { return per_present(double(bytes) / 1024.0); };
  const unsigned slice = unsigned(DiscardOutcome::NextSlice), pooled = unsigned(DiscardOutcome::Pooled),
                 fresh = unsigned(DiscardOutcome::NewBuffer);
  lines.push_back("d3d11 discards per Present: " + pp(w.discards[0][slice]) + " next slice, " +
                  pp(w.discards[0][pooled]) + " pooled, " + pp(w.discards[0][fresh]) + " new buffers (" +
                  kb(w.discard_new_bytes[0]) + " KB); deferred " + pp(w.discards[1][slice]) + ", " +
                  pp(w.discards[1][pooled]) + ", " + pp(w.discards[1][fresh]) + " (" + kb(w.discard_new_bytes[1]) +
                  " KB)");

  uint64_t repeated = 0;
  std::vector<unsigned> busiest;
  for (unsigned m = 0; m < kD11ContextMethods; m++) {
    repeated += w.repeated[m];
    if (w.repeated[m])
      busiest.push_back(m);
  }
  const size_t four = std::min<size_t>(4, busiest.size());
  std::partial_sort(busiest.begin(), busiest.begin() + four, busiest.end(), [&w](unsigned a, unsigned b) {
    if (w.repeated[a] != w.repeated[b])
      return w.repeated[a] > w.repeated[b];
    return std::strcmp(kD11CallNames[a], kD11CallNames[b]) < 0;
  });
  std::string state = "d3d11 state per Present: " + pp(repeated) + " repeated calls (";
  for (size_t k = 0; k < four; k++)
    state += std::string(k ? ", " : "") + kD11CallNames[busiest[k]] + " " + pp(w.repeated[busiest[k]]);
  state += "), tables " + pp(w.tables_cb) + " constant-buffer, " + pp(w.tables_srv) + " shader-resource, " +
           pp(w.tables_vb) + " vertex-buffer, " + pp(w.pipeline_lookups) + " pipeline look-ups";
  lines.push_back(std::move(state));

  std::string passes = "d3d11 passes per Present: " + pp(s.render) + " render, " + pp(s.compute) + " compute, " +
                       pp(s.blit) + " blit, " + pp(s.clear) + " clear; folded " + pp(s.folded_clears) +
                       " clears; merged " + pp(s.merged_render) + " render, " + pp(s.merged_compute) +
                       " compute, " + pp(s.merged_blit) + " blit; started after:";
  for (unsigned r = 0; r < unsigned(PassStart::Count); r++)
    passes += std::string(r ? ", " : " ") + kPassStartNames[r] + " " + pp(w.pass_starts[r]);
  lines.push_back(std::move(passes));

  std::string cuts = "d3d11 cuts per Present:";
  for (unsigned c = 0; c < unsigned(Cut::Count); c++)
    cuts += std::string(c ? ", " : " ") + kCutNames[c] + " " + pp(s.cut_applied[c]) + "/" + pp(s.cut_would[c]);
  auto mb = [&](uint64_t bytes) { return per_present(double(bytes) / (1024.0 * 1024.0)); };
  cuts += "; " + mb(s.bytes_avoided) + " MB avoided, " + mb(s.bytes_avoidable) + " MB more with the switches on";
  lines.push_back(std::move(cuts));

  if (p.level >= 2)
    lines.push_back("d3d11 time per Present: " + ms_per_present(ticks_ns(p.present_thread_ticks)) +
                    " ms in calls on the presenting thread, " + ms_per_present(ticks_ns(p.other_thread_ticks)) +
                    " ms on other threads, " + ms_per_present(double(s.encode_ns)) + " ms encoding");
  return lines;
}

/* ---- the table ---- */

/* The counters of the current period. count() is the hot path: a plain, non-atomic 32-bit add;
   counts lost to threads racing on the same counter are accepted. Only the period's start is
   atomic. */
class D11CallTable {
public:
  void
  count(unsigned id) {
    counts_[id] += 1;
  }

  void
  addTicks(unsigned id, uint64_t ticks, bool presenting_thread) {
    ticks_[id] += ticks;
    (presenting_thread ? present_thread_ticks_ : other_thread_ticks_) += ticks;
  }

  void
  countOtherThread() {
    other_thread_ += 1;
  }

  void
  countPresent() {
    presents_ += 1;
  }

  D11Work &
  work() {
    return work_;
  }

  void
  startIfIdle(uint64_t now_ns) {
    if (start_ns_.load(std::memory_order_relaxed) != 0)
      return;
    uint64_t idle = 0;
    start_ns_.compare_exchange_strong(idle, now_ns ? now_ns : 1, std::memory_order_relaxed);
  }

  bool
  reportDue(uint64_t now_ns) const {
    const uint64_t start = start_ns_.load(std::memory_order_relaxed);
    return start != 0 && now_ns >= start && now_ns - start >= kD11ReportPeriodNs;
  }

  /* Ends the period at `now_ns`: its report lines (with the encoder's `passes`), then every
     counter reset and the next period started at `now_ns`. One thread at a time. */
  std::vector<std::string>
  takeReport(uint64_t now_ns, const D11Passes &passes, unsigned level, uint64_t tick_hz) {
    D11Period period;
    std::vector<uint32_t> counts(counts_, counts_ + kD11CallCount);
    std::vector<uint64_t> ticks(ticks_, ticks_ + kD11CallCount);
    D11Work work = work_;
    period.counts = counts.data();
    period.ticks = ticks.data();
    period.tick_hz = tick_hz;
    period.other_thread = other_thread_;
    period.presents = presents_;
    period.work = &work;
    period.passes = &passes;
    period.present_thread_ticks = present_thread_ticks_;
    period.other_thread_ticks = other_thread_ticks_;
    period.level = level;
    const uint64_t start = start_ns_.load(std::memory_order_relaxed);
    period.elapsed_ns = now_ns > start ? now_ns - start : 0;
    std::memset(counts_, 0, sizeof(counts_));
    std::memset(ticks_, 0, sizeof(ticks_));
    work_ = D11Work{};
    other_thread_ = presents_ = 0;
    present_thread_ticks_ = other_thread_ticks_ = 0;
    start_ns_.store(now_ns ? now_ns : 1, std::memory_order_relaxed);
    return FormatD11Report(period);
  }

  uint32_t
  countOf(unsigned id) const {
    return counts_[id];
  }

  uint32_t
  presents() const {
    return presents_;
  }

private:
  uint32_t counts_[kD11CallCount] = {};
  uint64_t ticks_[kD11CallCount] = {};
  D11Work work_;
  uint32_t other_thread_ = 0;
  uint32_t presents_ = 0;
  uint64_t present_thread_ticks_ = 0, other_thread_ticks_ = 0;
  std::atomic<uint64_t> start_ns_{0};
};

} // namespace dxmt::d11cs
