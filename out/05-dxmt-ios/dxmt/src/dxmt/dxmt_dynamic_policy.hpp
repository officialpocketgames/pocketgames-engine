// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Map(WRITE_DISCARD) on dynamic buffers: the rules, kept free of Metal and
// Windows types so the host tests and the benchmark (tests/bench_dynamic_discard.cpp)
// build them as they are.
//
// A discard hands the buffer a fresh slice of memory. Slices live in
// allocations of several slices each (BufferAllocation's suballocations), so
// most discards only move to the next slice. A buffer whose discards outpace
// the GPU (a new allocation is needed while the previous one was made in the
// same command chunk) doubles the slices of its next allocations, up to
// kDynamicAllocationTargetBytes per allocation; a buffer discarded now and then
// keeps one slice.
//
// Memory stays bounded: every kDefaultDynamicCommitBytes of discarded slices
// the context commits its chunk, so the GPU starts on the work and the slices
// it read come back; and when more than kDefaultDynamicInflightBytes of
// discarded slices are in committed chunks the GPU hasn't finished, the next
// discard waits for the oldest of them.

#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

namespace dxmt {

constexpr uint64_t kDynamicSliceAlignment = 256;
constexpr uint64_t kDynamicAllocationTargetBytes = 1u << 20; // 1 MiB
constexpr uint64_t kDefaultDynamicCommitBytes = 32u << 20;   // 32 MiB
constexpr uint64_t kDefaultDynamicInflightBytes = 96u << 20; // 96 MiB

/* Bytes one slice of a buffer of `length` bytes takes. */
inline uint64_t
DynamicSliceBytes(uint64_t length) {
  const uint64_t l = std::max<uint64_t>(length, 256);
  return (l + kDynamicSliceAlignment - 1) & ~(kDynamicSliceAlignment - 1);
}

/* The most slices an allocation of a buffer of `length` bytes gets. */
inline uint32_t
MaxDynamicSlices(uint64_t length) {
  const uint64_t slice = DynamicSliceBytes(length);
  return slice >= kDynamicAllocationTargetBytes ? 1u : uint32_t(kDynamicAllocationTargetBytes / slice);
}

/* Slices for the buffer's next new allocations: doubled (from 1 to 2, 2 to 4,
   ...) when a new allocation is needed in the same chunk as the previous one,
   unchanged otherwise, never above MaxDynamicSlices. */
inline uint32_t
NextDynamicSlices(uint32_t current, bool minted_in_same_chunk, uint64_t length) {
  const uint32_t cap = MaxDynamicSlices(length);
  uint32_t next = std::max<uint32_t>(current, 1);
  if (minted_in_same_chunk)
    next = next >= cap ? cap : std::min<uint32_t>(next * 2, cap);
  return std::min(next, cap);
}

/* Discarded bytes per command chunk, for the implicit commit and the in-flight
   cap. Used by one context, under its lock. */
class DynamicDiscardTracker {
public:
  DynamicDiscardTracker(
      uint64_t commit_bytes = kDefaultDynamicCommitBytes, uint64_t inflight_bytes = kDefaultDynamicInflightBytes
  ) :
      commit_bytes_(commit_bytes),
      inflight_cap_(inflight_bytes) {}

  void
  noteDiscard(uint64_t slice_bytes) {
    since_commit_ += slice_bytes;
  }

  /* Before the next discard: commit the current chunk now? */
  bool
  commitDue() const {
    return commit_bytes_ != 0 && since_commit_ >= commit_bytes_;
  }

  /* The chunk with sequence number `seq` was committed. */
  void
  onCommit(uint64_t seq) {
    if (since_commit_ == 0)
      return;
    inflight_.push_back({seq, since_commit_});
    inflight_bytes_ += since_commit_;
    since_commit_ = 0;
  }

  /* Chunks up to `coherent_seq` have finished on the GPU. */
  void
  retire(uint64_t coherent_seq) {
    while (!inflight_.empty() && inflight_.front().seq <= coherent_seq) {
      inflight_bytes_ -= inflight_.front().bytes;
      inflight_.pop_front();
    }
  }

  /* The chunk to wait for so that the bytes still in flight (committed and
     not yet in the current chunk) are back under the cap; 0 when nothing has
     to wait. Wait for it (the GPU finishing that chunk), then retire(). */
  uint64_t
  waitSeq(uint64_t coherent_seq) {
    retire(coherent_seq);
    if (inflight_cap_ == 0 || inflight_bytes_ <= inflight_cap_)
      return 0;
    uint64_t remaining = inflight_bytes_;
    for (const auto &entry : inflight_) {
      remaining -= entry.bytes;
      if (remaining <= inflight_cap_)
        return entry.seq;
    }
    return inflight_.back().seq;
  }

  /* An implicit commit is made: returns how many this context has made. */
  uint64_t
  noteImplicitCommit() {
    return ++implicit_commits_;
  }

  /* Log the n-th implicit commit? The first, then every 600th (a running
     count, so the device log shows how many pass breaks a frame costs). */
  static bool
  logImplicitCommit(uint64_t n) {
    return n == 1 || (n != 0 && n % 600 == 0);
  }

  uint64_t
  commitBytes() const {
    return commit_bytes_;
  }

  uint64_t
  sinceCommit() const {
    return since_commit_;
  }

  uint64_t
  inflightBytes() const {
    return inflight_bytes_;
  }

private:
  struct Entry {
    uint64_t seq;
    uint64_t bytes;
  };

  uint64_t commit_bytes_;
  uint64_t inflight_cap_;
  uint64_t since_commit_ = 0;
  uint64_t inflight_bytes_ = 0;
  uint64_t implicit_commits_ = 0;
  std::deque<Entry> inflight_;
};

} // namespace dxmt
