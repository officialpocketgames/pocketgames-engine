// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 2: the guest memory of a 32-bit program's Direct3D 11 path. In the i386 build
// (spec 06 B) every CPU-visible Metal buffer and every staging ring block brings memory from the
// DLL's heap, which lies in the program's 4 GB guest window. A build of test 104 used it up: a
// game made ~1000 allocations of 2 MB in 0.1 s and kept them.
//
// Placed memory goes through Alloc / Free here, sorted by use (maps: the memory of buffers and
// textures the program maps; staging: the staging rings; other). The ledger counts blocks and
// bytes; in the i386 build a line is logged every 10 s:
//   [dxmt-wow] guest memory: <n> blocks, <MB> MB (maps <MB>, staging <MB>, other <MB>)
// and once, at the first allocation that fails:
//   [dxmt-wow] guest allocation of <size> failed: <what>
// A failed allocation is also counted per thread (ThreadFailures), so an API call can tell that
// one failed while it ran and return E_OUTOFMEMORY instead of writing through null.
//
// The ledger and the lines are pure (the host test builds them); util_guest_memory.cpp does
// the allocation and the logging.

#pragma once

#include "util_bitness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dxmt::guest_memory {

/* Placed memory comes from the DLL's own heap inside the program's guest window: the i386 build
   (not the ios_wow_native one, whose memory comes from spec 33's allocator) */
constexpr bool kGuestWindowHeap = kI386Build && !kIosWowNative;

enum class Use : unsigned { Maps, Staging, Other, Count };

constexpr uint64_t kReportPeriodNs = 10ull * 1000 * 1000 * 1000;

struct Snapshot {
  uint64_t blocks = 0;
  uint64_t bytes[unsigned(Use::Count)] = {};
};

inline std::string
Megabytes(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", double(bytes) / (1024.0 * 1024.0));
  return text;
}

inline std::string
ReportLine(const Snapshot &s) {
  uint64_t total = 0;
  for (uint64_t b : s.bytes)
    total += b;
  return "[dxmt-wow] guest memory: " + std::to_string(s.blocks) + " blocks, " + Megabytes(total) + " MB (maps " +
         Megabytes(s.bytes[unsigned(Use::Maps)]) + ", staging " + Megabytes(s.bytes[unsigned(Use::Staging)]) +
         ", other " + Megabytes(s.bytes[unsigned(Use::Other)]) + ")";
}

inline std::string
FailureLine(uint64_t size, const char *what) {
  return "[dxmt-wow] guest allocation of " + std::to_string(size) + " failed: " + (what ? what : "memory");
}

/* The counts and the report period; atomics, so any thread may count. */
class Ledger {
public:
  void
  add(Use use, uint64_t bytes) {
    blocks_.fetch_add(1, std::memory_order_relaxed);
    bytes_[unsigned(use)].fetch_add(bytes, std::memory_order_relaxed);
  }

  void
  remove(Use use, uint64_t bytes) {
    blocks_.fetch_sub(1, std::memory_order_relaxed);
    bytes_[unsigned(use)].fetch_sub(bytes, std::memory_order_relaxed);
  }

  Snapshot
  snapshot() const {
    Snapshot s;
    s.blocks = blocks_.load(std::memory_order_relaxed);
    for (unsigned u = 0; u < unsigned(Use::Count); u++)
      s.bytes[u] = bytes_[u].load(std::memory_order_relaxed);
    return s;
  }

  /* true at most once per kReportPeriodNs: the first call starts the period, and each call
     10 s or more after the period started takes the line and starts the next one */
  bool
  reportDue(uint64_t now_ns) {
    uint64_t start = start_ns_.load(std::memory_order_relaxed);
    if (start == 0) {
      start_ns_.compare_exchange_strong(start, now_ns ? now_ns : 1, std::memory_order_relaxed);
      return false;
    }
    if (now_ns < start || now_ns - start < kReportPeriodNs)
      return false;
    return start_ns_.compare_exchange_strong(start, now_ns, std::memory_order_relaxed);
  }

  /* true for the first failure only */
  bool
  takeFailureLine() {
    return !failure_logged_.exchange(true, std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> blocks_{0};
  std::atomic<uint64_t> bytes_[unsigned(Use::Count)] = {};
  std::atomic<uint64_t> start_ns_{0};
  std::atomic<bool> failure_logged_{false};
};

/* ---- util_guest_memory.cpp ---- */

Ledger &ProcessLedger();

/* Placed memory of `bytes` (already rounded by the caller), aligned to `alignment`, counted
   under `use`; null when the heap has none left: the failure line is logged once and the
   calling thread's failure count goes up. */
void *Alloc(size_t bytes, size_t alignment, Use use, const char *what);
/* `bytes` and `use` as given to Alloc; a null `ptr` is ignored. */
void Free(void *ptr, size_t bytes, Use use);
/* Failed allocations on this thread so far: an API call compares it before and after. */
uint64_t ThreadFailures();
/* In the i386 build, logs the report line when 10 s have passed (called by Alloc, Free and each
   Present). */
void Tick();

/* ---- util_guest_memory_log.cpp (the host benchmark has its own) ---- */

/* One line through the logger: `error` for the failure line, info for the report. */
void Write(const std::string &line, bool error);

} // namespace dxmt::guest_memory
