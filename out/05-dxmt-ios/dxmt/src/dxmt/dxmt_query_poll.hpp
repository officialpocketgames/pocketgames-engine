// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Query polling, for both front ends: spec 07 C7 and its clarification 2 (Direct3D 9, moved here
// from d3d9_ios_policy.hpp unchanged) and spec 05 round 18 (Direct3D 11): a program that polls a
// query in a loop parks from the third not-ready poll of a run instead of spinning a core. Pure:
// the host tests drive it with their own clocks and queues.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace dxmt {

/* ---- the run (spec 07 clarification 2) ---- */

constexpr uint64_t kQueryPollRunGapNs = 250000; // 250 us
constexpr uint64_t kQueryParkNs = 100000;       // 100 us: Direct3D 9's park
/* the first two not-ready polls of a run return at once; the third and later ones park */
constexpr uint32_t kQueryPollsBeforePark = 2;

/* Per-query polling state, reset when the query ends again. */
struct QueryPollState {
  uint64_t last_not_ready_ns = 0; // when the previous poll found the result not ready
  uint32_t run = 0;               // not-ready polls in the current run (adaptive)
  uint32_t polls_since_issue = 0; // every not-ready poll since Issue (non-adaptive)
  bool flushed_since_issue = false;

  void
  onIssueEnd() {
    *this = QueryPollState{};
  }
};

/* A poll found the result not ready at `now_ns` (only such polls call this; a ready poll never
   reads the clock): park the caller before answering? Adaptive: the gap to the previous
   not-ready poll is measured start to start (so that poll's park counts in it); a gap of at most
   kQueryPollRunGapNs continues the run, a longer one starts a new run at 1. Not adaptive: the
   count is every not-ready poll since Issue. Either way, park once the count is more than
   kQueryPollsBeforePark. */
inline bool
QueryPollShouldPark(QueryPollState &state, uint64_t now_ns, bool adaptive) {
  state.polls_since_issue++;
  if (!adaptive)
    return state.polls_since_issue > kQueryPollsBeforePark;
  const bool continues =
      state.run != 0 && now_ns >= state.last_not_ready_ns && now_ns - state.last_not_ready_ns <= kQueryPollRunGapNs;
  state.run = continues ? state.run + 1 : 1;
  state.last_not_ready_ns = now_ns;
  return state.run > kQueryPollsBeforePark;
}

/* The forward-progress submit a Direct3D 9 poll asks for happens once per Issue(END): true the
   first time after each Issue(END). */
inline bool
QueryTakeFlush(QueryPollState &state) {
  if (state.flushed_since_issue)
    return false;
  state.flushed_since_issue = true;
  return true;
}

/* ---- Direct3D 11 (spec 05 round 18) ---- */

constexpr uint64_t kD11QueryParkNs = 1000000; // 1 ms: item 80's longest park

/* Item 80: what a query parks on, kept with the query from its End: the poll state and the
   chunk that holds the end (0: never ended, nothing to wait for). */
struct QueryParkInfo {
  QueryPollState poll;
  uint64_t chunk_seq = 0;

  void
  ended(uint64_t chunk) {
    poll.onIssueEnd();
    chunk_seq = chunk;
  }
};

/* Item 80: the chunk an event-type query waits for. At End, a query with work recorded since the
   last signal gets a new signal in the chunk being recorded; one with nothing recorded reuses the
   last signal and waits for that signal's chunk (0 when there was none: the query is done). */
class EventSignalChunk {
public:
  uint64_t
  onEnd(bool new_signal, uint64_t recording_chunk) {
    if (new_signal)
      last_ = recording_chunk;
    return last_;
  }

private:
  uint64_t last_ = 0;
};

/* Item 80: what one GetData did, for item 83's line */
struct QueryPollResult {
  bool ready = false;
  bool flushed = false;  // the flush's body had something to commit
  bool parked = false;
  uint64_t parked_ns = 0;
};

/* Item 80, the order in GetData: check; when not ready, flush as today (unless DONOTFLUSH), so
   the chunk is on its way before the wait; park when the run says so; check once more and return
   that result. A poll parks only when the query's chunk has been committed (`committed(chunk)`):
   with DONOTFLUSH as item 80 says; without it the flush has just committed it, so there the test
   is a guard (QUESTIONS.md, round 18, choice 2). Without the switch, nothing parks and no clock
   is read.
     check()            true when the result is ready (and copies it); an error counts as ready;
     donotflush()       whether DONOTFLUSH is set, asked after the first check (the 64-poll stall
                        rule may have dropped it);
     flush()            the flush's body, true when it committed something;
     clock()            now, in nanoseconds (read only by a not-ready poll);
     committed(seq)     whether the chunk `seq` was committed;
     park(seq, ns)      waits until the chunk `seq` completes, or `ns` pass. */
template <typename Check, typename DoNotFlush, typename Flush, typename Clock, typename Committed, typename Park>
QueryPollResult
QueryPollFlow(
    QueryParkInfo &info, bool park_switch, Check &&check, DoNotFlush &&donotflush, Flush &&flush, Clock &&clock,
    Committed &&committed, Park &&park
) {
  QueryPollResult r;
  if (check()) {
    r.ready = true;
    return r;
  }
  if (!donotflush())
    r.flushed = flush();
  if (!park_switch || !info.chunk_seq)
    return r;
  const uint64_t now = clock();
  if (!QueryPollShouldPark(info.poll, now, true))
    return r;
  if (!committed(info.chunk_seq))
    return r; // no wait can make the result come
  r.parked = true;
  park(info.chunk_seq, kD11QueryParkNs);
  const uint64_t after = clock();
  r.parked_ns = after > now ? after - now : 0;
  r.ready = check();
  return r;
}

/* ---- item 82: an event poll asks Metal at most once ---- */

/* The highest shared-event value read so far: a value at or below it needs no call to the unix
   side; a poll that has to ask stores what it read. */
class EventValueCache {
public:
  template <typename Read>
  uint64_t
  atLeast(uint64_t wanted, Read &&read) {
    uint64_t known = known_.load(std::memory_order_acquire);
    if (wanted <= known)
      return known;
    uint64_t value = read();
    uint64_t current = known_.load(std::memory_order_relaxed);
    while (value > current && !known_.compare_exchange_weak(current, value, std::memory_order_release))
      ;
    return std::max(value, current);
  }

  uint64_t
  known() const {
    return known_.load(std::memory_order_acquire);
  }

private:
  std::atomic<uint64_t> known_{0};
};

inline uint64_t
QueryNowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
  );
}

} // namespace dxmt
