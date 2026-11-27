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
//
// Spec 06 round 3 (item 2): a fourth use, commands (the command rings' host blocks, the encoder's
// heap, the resource initializer's heap); the live ring and command blocks counted by size and
// added to the line; the line also at the first failure and each time the total passes another
// 256 MB (a 4-second run never reaches the 10 s line):
//   [dxmt-wow] guest memory: <n> blocks, <MB> MB (maps <MB>, staging <MB>, commands <MB>, other
//   <MB>); ring and command blocks: <MB> MB x <n>, …
//
// Spec 06 round 4 (item 4): the deferred command lists and the immediate context counted too,
// and added to the line once there is any:
//   …; command lists: <n> made, <n> alive, <n> waiting in chunks, <n> pooled; <n> presents, <n>
//   commits
// (alive: made and not in the pool: recording, held by the program or waiting in a chunk).

#pragma once

#include "util_bitness.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dxmt::guest_memory {

/* Placed memory comes from the DLL's own heap inside the program's guest window: the i386 build
   (not the ios_wow_native one, whose memory comes from spec 33's allocator) */
constexpr bool kGuestWindowHeap = kI386Build && !kIosWowNative;

enum class Use : unsigned { Maps, Staging, Commands, Other, Count };

constexpr uint64_t kReportPeriodNs = 10ull * 1000 * 1000 * 1000;
/* spec 06 round 3: the line again each time the total passes another this many bytes */
constexpr uint64_t kReportStepBytes = 256ull << 20;

/* spec 06 round 3: live ring and command blocks (staging and commands) of one size */
struct SizeCount {
  uint64_t size = 0;
  uint64_t count = 0;
};
constexpr unsigned kSizeSlots = 12;

/* spec 06 round 4 (item 4): what the deferred command lists and the immediate context did */
enum class ListCount : unsigned {
  Made,     // command lists made (the pool's instances)
  Pooled,   // in the pool, waiting to be reused
  Waiting,  // executed, held by a chunk that hasn't finished
  Presents, // Present calls (not DXGI_PRESENT_TEST)
  Commits,  // the immediate context's commits (Present, flush, backlog)
  Count
};

struct Snapshot {
  uint64_t blocks = 0;
  uint64_t bytes[unsigned(Use::Count)] = {};
  SizeCount sizes[kSizeSlots] = {};
  uint64_t lists[unsigned(ListCount::Count)] = {}; // spec 06 round 4
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
  std::string line = "[dxmt-wow] guest memory: " + std::to_string(s.blocks) + " blocks, " + Megabytes(total) +
                     " MB (maps " + Megabytes(s.bytes[unsigned(Use::Maps)]) + ", staging " +
                     Megabytes(s.bytes[unsigned(Use::Staging)]) + ", commands " +
                     Megabytes(s.bytes[unsigned(Use::Commands)]) + ", other " +
                     Megabytes(s.bytes[unsigned(Use::Other)]) + ")";
  /* spec 06 round 3: the live ring and command blocks by size, largest first */
  SizeCount sizes[kSizeSlots];
  unsigned n = 0;
  for (const SizeCount &c : s.sizes) {
    if (!c.count)
      continue;
    unsigned i = n++; // insertion, largest first (at most kSizeSlots)
    while (i > 0 && sizes[i - 1].size < c.size) {
      sizes[i] = sizes[i - 1];
      i--;
    }
    sizes[i] = c;
  }
  for (unsigned i = 0; i < n; i++)
    line += (i ? ", " : "; ring and command blocks: ") + Megabytes(sizes[i].size) + " MB x " +
            std::to_string(sizes[i].count);
  /* spec 06 round 4: the command lists, the presents and the commits */
  const uint64_t *l = s.lists;
  const uint64_t made = l[unsigned(ListCount::Made)], pooled = l[unsigned(ListCount::Pooled)];
  if (made || l[unsigned(ListCount::Presents)] || l[unsigned(ListCount::Commits)])
    line += "; command lists: " + std::to_string(made) + " made, " +
            std::to_string(made > pooled ? made - pooled : 0) + " alive, " +
            std::to_string(l[unsigned(ListCount::Waiting)]) + " waiting in chunks, " + std::to_string(pooled) +
            " pooled; " + std::to_string(l[unsigned(ListCount::Presents)]) + " presents, " +
            std::to_string(l[unsigned(ListCount::Commits)]) + " commits";
  return line;
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
    if (use == Use::Staging || use == Use::Commands)
      countSize(bytes, 1);
  }

  void
  remove(Use use, uint64_t bytes) {
    blocks_.fetch_sub(1, std::memory_order_relaxed);
    bytes_[unsigned(use)].fetch_sub(bytes, std::memory_order_relaxed);
    if (use == Use::Staging || use == Use::Commands)
      countSize(bytes, -1);
  }

  Snapshot
  snapshot() const {
    Snapshot s;
    s.blocks = blocks_.load(std::memory_order_relaxed);
    for (unsigned u = 0; u < unsigned(Use::Count); u++)
      s.bytes[u] = bytes_[u].load(std::memory_order_relaxed);
    lock();
    for (unsigned i = 0; i < kSizeSlots; i++)
      s.sizes[i] = sizes_[i];
    unlock();
    for (unsigned c = 0; c < unsigned(ListCount::Count); c++) // spec 06 round 4
      s.lists[c] = lists_[c].load(std::memory_order_relaxed);
    return s;
  }

  /* spec 06 round 4 (item 4): one use's bytes (the commands, for FinishCommandList's wait) */
  uint64_t
  bytesOf(Use use) const {
    return bytes_[unsigned(use)].load(std::memory_order_relaxed);
  }

  /* spec 06 round 4 (item 4): a command list or immediate-context count moves */
  void
  countList(ListCount count, int64_t delta) {
    lists_[unsigned(count)].fetch_add(uint64_t(delta), std::memory_order_relaxed);
  }

  uint64_t
  totalBytes() const {
    uint64_t total = 0;
    for (const auto &b : bytes_)
      total += b.load(std::memory_order_relaxed);
    return total;
  }

  /* spec 06 round 3: true once each time `total` reaches another kReportStepBytes */
  bool
  passedStep(uint64_t total) {
    uint64_t next = next_step_.load(std::memory_order_relaxed);
    if (total < next)
      return false;
    return next_step_.compare_exchange_strong(
        next, (total / kReportStepBytes + 1) * kReportStepBytes, std::memory_order_relaxed
    );
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
  /* spec 06 round 3: the size table under a spin lock (blocks of these uses are few and large) */
  void
  lock() const {
    while (size_lock_.test_and_set(std::memory_order_acquire)) {
    }
  }
  void
  unlock() const {
    size_lock_.clear(std::memory_order_release);
  }
  void
  countSize(uint64_t size, int delta) {
    lock();
    SizeCount *slot = nullptr, *empty = nullptr;
    for (auto &c : sizes_) {
      if (c.count && c.size == size) {
        slot = &c;
        break;
      }
      if (!c.count && !empty)
        empty = &c;
    }
    if (!slot && delta > 0 && empty) {
      slot = empty;
      slot->size = size;
    }
    if (slot)
      slot->count = delta > 0 ? slot->count + 1 : (slot->count ? slot->count - 1 : 0);
    unlock();
  }

  std::atomic<uint64_t> blocks_{0};
  std::atomic<uint64_t> bytes_[unsigned(Use::Count)] = {};
  std::atomic<uint64_t> start_ns_{0};
  std::atomic<bool> failure_logged_{false};
  std::atomic<uint64_t> next_step_{kReportStepBytes};
  mutable std::atomic_flag size_lock_ = ATOMIC_FLAG_INIT;
  SizeCount sizes_[kSizeSlots] = {};
  std::atomic<uint64_t> lists_[unsigned(ListCount::Count)] = {}; // spec 06 round 4
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
