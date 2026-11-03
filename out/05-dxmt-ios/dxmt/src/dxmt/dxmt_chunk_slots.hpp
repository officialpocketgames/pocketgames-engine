// SPDX-License-Identifier: LGPL-2.1-or-later
// The reservation loop is from steamac's DXMT patch 0019 (时雨, "fix(dxmt): reserve command slots
// before publishing chunks", gamesir-labs/dxmt a350b20). Moved into this header, with the release
// half, for MYIOSDECK spec 05 clarification 10 so it can be tested on a host
// (tests/test_chunk_slots.cpp; see LICENSE-NOTES.md). Copyright (c) 2026 the MYIOSDECK author for
// the move.
#pragma once

#include "util_atomic_wait.hpp"
#include <atomic>
#include <cstdint>

namespace dxmt {

/* Counts a command chunk in before it is published: waits while `limit` chunks are
   in flight, then takes a slot with a compare-exchange. CommandQueue reserves first
   and publishes after, so the finish thread can't release a chunk's slot before it
   was taken (the counter never goes below zero), and no more than `limit` chunks
   are ever in flight. One thread reserves (the one that commits chunks). */
inline void
ReserveChunkSlot(std::atomic_uint64_t &ongoing, uint64_t limit) {
  for (;;) {
    auto current = ongoing.load(std::memory_order_acquire);
    while (current >= limit) {
      atomic_wait(ongoing, current, std::memory_order_acquire);
      current = ongoing.load(std::memory_order_acquire);
    }
    if (ongoing.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel, std::memory_order_acquire))
      return;
  }
}

/* The finish thread's half: a finished chunk gives its slot back and wakes a waiting
   ReserveChunkSlot. Returns the count before the release (at least 1). */
inline uint64_t
ReleaseChunkSlot(std::atomic_uint64_t &ongoing) {
  auto before = ongoing.fetch_sub(1, std::memory_order_release);
  atomic_notify_one(ongoing);
  return before;
}

} // namespace dxmt
