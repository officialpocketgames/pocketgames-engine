// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 4 (item 4): deferred command lists must not use up a 32-bit address space. A
// list's blocks go back to the pool only when the list is recycled: after the program has
// released it and the chunk that executed it was committed and finished. A game that finished
// ~1,900 lists in 0.1 s before its first Present committed no chunk, so none came back.
// - The backlog: the lists and their bytes executed since the immediate context last committed.
//   Past kBacklogLists lists or kBacklogBytes bytes, ExecuteCommandList commits the current chunk
//   (as a flush does), so finished chunks recycle their lists without a Present. Both bitnesses.
// - In a 32-bit process, past kCommandWaitBytes of command memory in the guest-memory ledger,
//   FinishCommandList waits for the oldest committed chunk to finish before it records more.
// - The lines: the first backlog commit and the first FinishCommandList that fails.
// Pure, so the host tests build it as it is (tests/test_guest_memory.cpp, the benchmark's check).

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace dxmt {

constexpr unsigned kBacklogLists = 8;
constexpr uint64_t kBacklogBytes = 16ull << 20;
constexpr uint64_t kCommandWaitBytes = 1ull << 30;

inline std::string
BacklogMegabytes(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", double(bytes) / (1024.0 * 1024.0));
  return text;
}

/* The immediate context's count (used under its lock). */
class CommandListBacklog {
public:
  /* A list holding `bytes` of command memory was executed; true when the chunk is to be
     committed now: more than kBacklogLists lists, or more than kBacklogBytes, since the last
     commit. */
  bool
  executed(uint64_t bytes) {
    lists_++;
    bytes_ += bytes;
    return lists_ > kBacklogLists || bytes_ > kBacklogBytes;
  }

  /* The immediate context committed (a Present, a flush, a backlog commit) */
  void
  committed() {
    lists_ = 0;
    bytes_ = 0;
  }

  unsigned
  lists() const {
    return lists_;
  }

  uint64_t
  bytes() const {
    return bytes_;
  }

  /* The line of the process's first backlog commit, or "" after it (call before the commit) */
  std::string
  takeFirstLine() {
    if (logged_)
      return std::string();
    logged_ = true;
    return "[d3d11] command list backlog: " + std::to_string(lists_) + " lists, " + BacklogMegabytes(bytes_) +
           " MB since the last commit; committing";
  }

private:
  unsigned lists_ = 0;
  uint64_t bytes_ = 0;
  bool logged_ = false;
};

/* FinishCommandList waits for the oldest committed chunk first: a 32-bit process past the mark */
constexpr bool
CommandMemoryWaitDue(bool i386, uint64_t command_bytes, uint64_t mark = kCommandWaitBytes) {
  return i386 && command_bytes > mark;
}

/* The chunk to wait for: the oldest one committed and not finished, or 0 when every committed
   chunk has finished (there is nothing to wait for; the chunk being recorded isn't committed) */
constexpr uint64_t
OldestUnfinishedChunk(uint64_t coherent_seq, uint64_t current_seq) {
  return coherent_seq + 1 < current_seq ? coherent_seq + 1 : 0;
}

inline std::string
CommandWaitLine(uint64_t command_bytes) {
  return "[d3d11] command memory " + BacklogMegabytes(command_bytes) +
         " MB, past 1024 MB: FinishCommandList waits for the oldest committed chunk";
}

inline std::string
FinishOutOfMemoryLine(uint64_t command_bytes) {
  return "[d3d11] FinishCommandList -> E_OUTOFMEMORY: a command block couldn't be had (commands " +
         BacklogMegabytes(command_bytes) + " MB)";
}

} // namespace dxmt
