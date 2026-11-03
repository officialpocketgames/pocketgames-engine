// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 10: host test of the command-slot reservation from steamac's DXMT
// patch 0019 (dxmt/src/dxmt/dxmt_chunk_slots.hpp, as CommandQueue::CommitCurrentChunk and its
// finish thread use it). A committer reserves a slot, then publishes the chunk; a finisher
// takes published chunks in order and releases their slots. Checked: the reservation blocks
// at the limit and resumes on a release; under load the count never goes below zero (a
// release never comes before its chunk's reservation, which upstream's publish-then-count
// order allowed), never exceeds the limit, and a chunk is never recorded into a slot whose
// previous chunk is still in flight.
//
//   c++ -std=c++20 -pthread -I../dxmt/src/dxmt -I../dxmt/src/util test_chunk_slots.cpp

#include "test_common.hpp"

#define DXMT_ATOMIC_WAIT_GENERIC 1
#include "dxmt_chunk_slots.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace dxmt {
void
atomic_wait_log_backend() {}
} // namespace dxmt

namespace {

using namespace std::chrono_literals;

void
TestReserveBlocksAtLimit() {
  std::atomic_uint64_t ongoing{0};
  dxmt::ReserveChunkSlot(ongoing, 2);
  dxmt::ReserveChunkSlot(ongoing, 2);
  CHECK_EQ(ongoing.load(), 2);

  std::atomic<bool> reserved{false};
  std::thread committer([&] {
    dxmt::ReserveChunkSlot(ongoing, 2);
    reserved.store(true);
  });
  std::this_thread::sleep_for(50ms);
  CHECK(!reserved.load()); // both slots are in flight: it waits
  CHECK_EQ(dxmt::ReleaseChunkSlot(ongoing), 2);
  committer.join();
  CHECK(reserved.load());
  CHECK_EQ(ongoing.load(), 2);

  CHECK_EQ(dxmt::ReleaseChunkSlot(ongoing), 2);
  CHECK_EQ(dxmt::ReleaseChunkSlot(ongoing), 1);
  CHECK_EQ(ongoing.load(), 0);
}

/* The CommandQueue shape: 32 chunk slots, at most 31 chunks in flight, chunk ids from 1. */
void
TestCommitAndFinishUnderLoad() {
  constexpr uint64_t kSlots = 32;
  constexpr uint64_t kLimit = kSlots - 1;
  constexpr uint64_t kChunks = 20000;
  enum : int { Free = 0, Recording = 1, InFlight = 2 };

  std::array<std::atomic<int>, kSlots> slot_state{};
  std::atomic_uint64_t ongoing{0};
  std::atomic_uint64_t published{0};
  std::atomic<uint64_t> slot_reused_in_flight{0}, over_limit{0}, released_unreserved{0}, finished_wrong{0};
  std::atomic<uint64_t> max_ongoing{0};

  std::thread finisher([&] {
    for (uint64_t id = 1; id <= kChunks; id++) {
      for (;;) {
        auto seen = published.load(std::memory_order_acquire);
        if (seen >= id)
          break;
        dxmt::atomic_wait(published, seen, std::memory_order_acquire);
      }
      if (id % 7 == 0)
        std::this_thread::yield(); // the GPU takes a while now and then
      if (slot_state[id % kSlots].exchange(Free) != InFlight)
        finished_wrong++;
      if (dxmt::ReleaseChunkSlot(ongoing) == 0)
        released_unreserved++; // would wrap below zero
    }
  });

  for (uint64_t id = 1; id <= kChunks; id++) {
    // recording into the chunk's slot: its previous chunk must have finished
    if (slot_state[id % kSlots].exchange(Recording) != Free)
      slot_reused_in_flight++;
    dxmt::ReserveChunkSlot(ongoing, kLimit);
    auto now = ongoing.load();
    if (now > kLimit)
      over_limit++;
    if (now > max_ongoing.load())
      max_ongoing.store(now);
    slot_state[id % kSlots].store(InFlight);
    published.store(id, std::memory_order_release);
    dxmt::atomic_notify_one(published);
  }
  finisher.join();

  CHECK_EQ(slot_reused_in_flight.load(), 0);
  CHECK_EQ(over_limit.load(), 0);
  CHECK_EQ(released_unreserved.load(), 0);
  CHECK_EQ(finished_wrong.load(), 0);
  CHECK(max_ongoing.load() >= 1 && max_ongoing.load() <= kLimit);
  CHECK_EQ(ongoing.load(), 0);
}

} // namespace

int
main() {
  TestReserveBlocksAtLimit();
  TestCommitAndFinishUnderLoad();
  return TestSummary("test_chunk_slots");
}
