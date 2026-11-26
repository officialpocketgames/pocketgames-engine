// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 2: placed memory through the guest-memory ledger (util_guest_memory.hpp).

#include "util_guest_memory.hpp"

#include "wsi_platform.hpp"

#include <chrono>

namespace dxmt::guest_memory {

namespace {

thread_local uint64_t t_failures = 0;

[[maybe_unused]] uint64_t // only the i386 build's Tick reads the clock
NowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
  );
}

} // namespace

Ledger &
ProcessLedger() {
  static Ledger ledger;
  return ledger;
}

void *
Alloc(size_t bytes, size_t alignment, Use use, const char *what) {
  void *ptr = wsi::aligned_malloc(bytes, alignment);
  if (!ptr) {
    t_failures++;
    if (ProcessLedger().takeFailureLine()) {
      Write(FailureLine(bytes, what), true);
      Write(ReportLine(ProcessLedger().snapshot()), false); // spec 06 round 3: what holds it
    }
    return nullptr;
  }
  ProcessLedger().add(use, bytes);
  /* spec 06 round 3: the line each time the total passes another 256 MB */
  if constexpr (kGuestWindowHeap) {
    if (ProcessLedger().passedStep(ProcessLedger().totalBytes()))
      Write(ReportLine(ProcessLedger().snapshot()), false);
  }
  Tick();
  return ptr;
}

void
Free(void *ptr, size_t bytes, Use use) {
  if (!ptr)
    return;
  wsi::aligned_free(ptr);
  ProcessLedger().remove(use, bytes);
}

uint64_t
ThreadFailures() {
  return t_failures;
}

void
Tick() {
  if constexpr (kGuestWindowHeap) {
    if (ProcessLedger().reportDue(NowNs()))
      Write(ReportLine(ProcessLedger().snapshot()), false);
  }
}

} // namespace dxmt::guest_memory
