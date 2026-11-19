// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 round 18 (item 85): a polled Direct3D 11 query parks.
//   - item 80: the run (dxmt/src/dxmt/dxmt_query_poll.hpp's QueryPollFlow, with a fake clock),
//     the chunk a query waits for (EventSignalChunk), the park against a fake queue (a real
//     CpuFence: returns as soon as the chunk completes, after 1 ms when it doesn't), DONOTFLUSH,
//     the switch off;
//   - item 81: a model of the immediate context's GetData and Flush with the real call counters
//     (d3d11_call_stats.hpp): a not-ready GetData counts one call, Flush none
//     (check-d3d11-call-sites.sh checks that the front end calls no public Flush);
//   - item 82: EventValueCache;
//   - item 83: a child call inside a context or device call counts as inside, one outside
//     doesn't (the lines themselves: test_d3d11_call_stats.cpp).
// Only the device can check the ntdll wait (the Windows half): here the generic backend runs.

#include "test_common.hpp"

#define DXMT_ATOMIC_WAIT_GENERIC 1
#include "dxmt_query_poll.hpp"
#include "util_cpu_fence.hpp"
#include "d3d11_call_stats.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

/* ---- fakes of what the test doesn't link ---- */

namespace dxmt {
void
atomic_wait_log_backend() {}
} // namespace dxmt

namespace dxmt::d11cs {
unsigned g_level = 1;
D11CallTable g_fake_table;
void
NoteCall(unsigned id) {
  g_fake_table.count(id);
}
void
AddTicks(unsigned, uint64_t) {}
D11Work &
Work() {
  return g_fake_table.work();
}
} // namespace dxmt::d11cs

namespace {

using namespace dxmt;
using namespace dxmt::d11cs;

constexpr uint64_t kUs = 1000;

/* A query, a fake clock and a fake queue for QueryPollFlow. */
struct Poller {
  QueryParkInfo info;
  bool ready = false;           // what check() answers
  bool ready_after_park = false; // a park completes the chunk: the second check is ready
  bool donotflush = false;
  bool committed = true;
  uint64_t now = 0;
  int clock_reads = 0, flushes = 0, checks = 0, parks = 0;
  uint64_t parked_seq = 0, parked_ns = 0;
  uint64_t park_takes = 50 * kUs;

  QueryPollResult
  poll(bool park_switch) {
    return QueryPollFlow(
        info, park_switch,
        [&] {
          checks++;
          return ready;
        },
        [&] { return donotflush; },
        [&] {
          flushes++;
          return true;
        },
        [&] {
          clock_reads++;
          return now;
        },
        [&](uint64_t) { return committed; },
        [&](uint64_t seq, uint64_t ns) {
          parks++;
          parked_seq = seq;
          parked_ns = ns;
          now += park_takes;
          if (ready_after_park)
            ready = true;
        }
    );
  }
};

void
TestRun() {
  Poller p;
  p.info.ended(7);
  // two not-ready polls return at once, the third parks
  p.now = 1000 * kUs;
  auto a = p.poll(true);
  p.now += 50 * kUs;
  auto b = p.poll(true);
  p.now += 50 * kUs;
  auto c = p.poll(true);
  CHECK(!a.parked && !b.parked && c.parked);
  CHECK(!a.ready && !b.ready && !c.ready);
  CHECK_EQ(p.parks, 1);
  CHECK_EQ(p.parked_seq, 7u);
  CHECK_EQ(p.parked_ns, kD11QueryParkNs); // 1 ms
  CHECK_EQ(c.parked_ns, 50 * kUs);
  CHECK_EQ(p.flushes, 3); // flushed as before, every not-ready poll
  CHECK(a.flushed && c.flushed);
  // the fourth, 10 us after the third's park ended (start to start: 60 us), parks too
  p.now += 10 * kUs;
  CHECK(p.poll(true).parked);
  // a gap of 251 us starts a new run at 1: two return, the third parks
  p.now += 251 * kUs;
  CHECK(!p.poll(true).parked);
  p.now += 250 * kUs; // exactly 250 us continues it
  CHECK(!p.poll(true).parked);
  p.now += 1 * kUs;
  CHECK(p.poll(true).parked);

  // a poll that finds the result ready reads no clock and flushes nothing
  Poller r;
  r.info.ended(3);
  r.ready = true;
  for (int i = 0; i < 5; i++) {
    auto res = r.poll(true);
    CHECK(res.ready && !res.parked && !res.flushed);
  }
  CHECK_EQ(r.clock_reads, 0);
  CHECK_EQ(r.flushes, 0);

  // a new End starts the run again
  p.info.ended(9);
  p.now += 1 * kUs;
  CHECK(!p.poll(true).parked);

  // switch off: never parks, reads no clock, flushes as before
  Poller off;
  off.info.ended(5);
  for (int i = 0; i < 10; i++) {
    off.now += kUs;
    CHECK(!off.poll(false).parked);
  }
  CHECK_EQ(off.parks, 0);
  CHECK_EQ(off.clock_reads, 0);
  CHECK_EQ(off.flushes, 10);

  // a query that never ended (chunk 0) waits for nothing
  Poller none;
  for (int i = 0; i < 5; i++)
    CHECK(!none.poll(true).parked);
}

void
TestSecondCheck() {
  // the result after the park is the second check's
  Poller p;
  p.info.ended(4);
  p.ready_after_park = true;
  p.poll(true);
  p.poll(true);
  auto third = p.poll(true);
  CHECK(third.parked && third.ready);
  CHECK_EQ(p.checks, 4); // one each, two for the parked poll
  // and a park that times out answers not ready
  Poller q;
  q.info.ended(4);
  q.poll(true);
  q.poll(true);
  auto timed_out = q.poll(true);
  CHECK(timed_out.parked && !timed_out.ready);
  CHECK_EQ(q.checks, 4);
}

void
TestDoNotFlush() {
  Poller p;
  p.info.ended(6);
  p.donotflush = true;
  p.committed = false;
  for (int i = 0; i < 6; i++) {
    p.now += kUs;
    CHECK(!p.poll(true).parked); // the chunk isn't committed: no wait can help
  }
  CHECK_EQ(p.flushes, 0);
  CHECK_EQ(p.parks, 0);
  p.committed = true; // once it is, the run parks
  p.now += kUs;
  CHECK(p.poll(true).parked);
  CHECK_EQ(p.flushes, 0);
  CHECK_EQ(p.parked_seq, 6u);
}

void
TestSignalChunk() {
  EventSignalChunk chunks;
  CHECK_EQ(chunks.onEnd(false, 3), 0u); // nothing recorded, no signal yet: nothing to wait for
  CHECK_EQ(chunks.onEnd(true, 5), 5u);  // a new signal: its own chunk
  CHECK_EQ(chunks.onEnd(false, 8), 5u); // nothing recorded since: the reused signal's chunk
  CHECK_EQ(chunks.onEnd(true, 9), 9u);
  QueryParkInfo info;
  info.poll.run = 4;
  info.ended(chunks.onEnd(false, 12));
  CHECK_EQ(info.chunk_seq, 9u);
  CHECK_EQ(info.poll.run, 0u); // a new End resets the run
}

void
TestFakeQueuePark() {
  using clock = std::chrono::steady_clock;
  // a park returns as soon as the chunk completes
  {
    CpuFence completed(1);
    std::thread gpu([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      completed.signal(2);
    });
    auto t0 = clock::now();
    CHECK(completed.waitFor(2, 10ull * 1000 * 1000 * 1000)); // 10 s allowed
    auto waited = clock::now() - t0;
    gpu.join();
    CHECK(waited < std::chrono::seconds(5));
  }
  // already complete: at once
  {
    CpuFence completed(3);
    CHECK(completed.waitFor(2, kD11QueryParkNs));
  }
  // and after 1 ms when it doesn't
  {
    CpuFence completed(1);
    auto t0 = clock::now();
    CHECK(!completed.waitFor(2, kD11QueryParkNs));
    auto waited = clock::now() - t0;
    CHECK(waited >= std::chrono::milliseconds(1));
    CHECK(waited < std::chrono::seconds(1));
  }
  // the flow against that queue: the third poll parks on the fence, the second check reads it
  {
    CpuFence completed(1);
    QueryParkInfo info;
    info.ended(2);
    uint64_t fake_now = 0;
    auto poll = [&] {
      return QueryPollFlow(
          info, true, [&] { return completed.signaledValue() >= 2; }, [] { return false; }, [] { return false; },
          [&] { return fake_now; }, [](uint64_t) { return true; },
          [&](uint64_t seq, uint64_t ns) { completed.waitFor(seq, ns); }
      );
    };
    CHECK(!poll().ready);
    CHECK(!poll().ready);
    std::thread gpu([&] { completed.signal(2); });
    gpu.join();
    auto third = poll(); // ready at the first check now
    CHECK(third.ready && !third.parked);
    CpuFence late(1);
    QueryParkInfo info2;
    info2.ended(2);
    auto poll2 = [&] {
      return QueryPollFlow(
          info2, true, [&] { return late.signaledValue() >= 2; }, [] { return false; }, [] { return false; },
          [&] { return fake_now; }, [](uint64_t) { return true; },
          [&](uint64_t seq, uint64_t ns) { late.waitFor(seq, ns); }
      );
    };
    poll2();
    poll2();
    std::thread gpu2([&] {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      late.signal(2);
    });
    auto parked = poll2(); // parks; ready if the signal came within the 1 ms, else not
    gpu2.join();
    CHECK(parked.parked);
    CHECK(!parked.ready || late.signaledValue() >= 2); // ready only from the second check's read
  }
}

void
TestEventCache() {
  EventValueCache cache;
  int reads = 0;
  uint64_t gpu = 5;
  auto read = [&] {
    reads++;
    return gpu;
  };
  CHECK_EQ(cache.atLeast(3, read), 5u); // has to ask, stores 5
  CHECK_EQ(reads, 1);
  CHECK_EQ(cache.known(), 5u);
  CHECK_EQ(cache.atLeast(5, read), 5u); // at the stored value: no call
  CHECK_EQ(cache.atLeast(2, read), 5u); // below it: no call
  CHECK_EQ(reads, 1);
  gpu = 9;
  CHECK_EQ(cache.atLeast(7, read), 9u); // higher: one call, stored
  CHECK_EQ(reads, 2);
  CHECK_EQ(cache.known(), 9u);
  CHECK_EQ(cache.atLeast(12, read), 9u); // not there yet: asks, answers below
  CHECK_EQ(reads, 3);
  CHECK_EQ(cache.known(), 9u);
  CHECK_EQ(cache.atLeast(8, read), 9u);
  CHECK_EQ(reads, 3);
}

/* Item 81: the immediate context's shape. GetData and Flush count themselves; GetData reaches
   the flush's body, not Flush. */
struct ModelContext {
  static constexpr unsigned kD11csBase = kD11IdImmediate;
  bool dirty = true;
  QueryParkInfo info;

  bool
  FlushUncounted() {
    if (!dirty)
      return false;
    dirty = false;
    return true;
  }

  void
  Flush() {
    D11_CTX_CALL(Flush);
    FlushUncounted();
  }

  bool
  GetData() {
    D11_CTX_CALL(GetData);
    auto r = QueryPollFlow(
        info, true, [] { return false; }, [] { return false; }, [&] { return FlushUncounted(); },
        [] { return uint64_t(0); }, [](uint64_t) { return true; }, [](uint64_t, uint64_t) {}
    );
    return r.ready;
  }
};

void
TestGetDataCounts() {
  g_fake_table.takeReport(1, D11Passes{}, 1, 1); // a fresh table
  ModelContext ctx;
  ctx.info.ended(2);
  CHECK(!ctx.GetData());
  CHECK_EQ(g_fake_table.countOf(kD11IdImmediate + D11X_GetData), 1u);
  CHECK_EQ(g_fake_table.countOf(kD11IdImmediate + D11X_Flush), 0u);
  CHECK(!ctx.dirty); // the body ran
  ctx.Flush();
  CHECK_EQ(g_fake_table.countOf(kD11IdImmediate + D11X_Flush), 1u);
}

/* Item 83: inside and outside child calls */
struct ModelDevice {
  void
  CreateBuffer() {
    D11_DEVICE_CALL(CreateBuffer);
    D11_CHILD_CALL(AddRef); // the front end's own reference
  }
};

void
TestInsideCalls() {
  g_fake_table.takeReport(1, D11Passes{}, 1, 1);
  D11_CHILD_CALL(AddRef); // the program's
  D11_CHILD_CALL(Release);
  CHECK_EQ(Work().child_inside[D11ChildIndex(D11O_Child_AddRef)], 0u);
  {
    ContextCall call(kD11IdImmediate + D11X_Draw);
    D11_CHILD_CALL(AddRef);
    D11_CHILD_CALL(Release);
    D11_CHILD_CALL(QueryInterface);
  }
  D11_CHILD_CALL(Release); // after the context call: outside again
  ModelDevice device;
  device.CreateBuffer();
  D11_CHILD_CALL(QueryInterface);
  CHECK_EQ(g_fake_table.countOf(kD11IdOther + D11O_Child_AddRef), 3u);
  CHECK_EQ(g_fake_table.countOf(kD11IdOther + D11O_Child_Release), 3u);
  CHECK_EQ(g_fake_table.countOf(kD11IdOther + D11O_Child_QueryInterface), 2u);
  CHECK_EQ(Work().child_inside[D11ChildIndex(D11O_Child_AddRef)], 2u);
  CHECK_EQ(Work().child_inside[D11ChildIndex(D11O_Child_Release)], 1u);
  CHECK_EQ(Work().child_inside[D11ChildIndex(D11O_Child_QueryInterface)], 1u);
  CHECK_EQ(g_fake_table.countOf(kD11IdDevice + D11D_CreateBuffer), 1u);
  CHECK_EQ(t_current_call, kD11CallCount); // every scope restored it
  // switch off: nothing counted, nothing inside
  g_level = 0;
  g_fake_table.takeReport(1, D11Passes{}, 1, 1);
  {
    ContextCall call(kD11IdImmediate + D11X_Draw);
    D11_CHILD_CALL(AddRef);
  }
  device.CreateBuffer();
  CHECK_EQ(g_fake_table.countOf(kD11IdOther + D11O_Child_AddRef), 0u);
  CHECK_EQ(Work().child_inside[D11ChildIndex(D11O_Child_AddRef)], 0u);
  g_level = 1;
}

} // namespace

int
main() {
  TestRun();
  TestSecondCheck();
  TestDoNotFlush();
  TestSignalChunk();
  TestFakeQueuePark();
  TestEventCache();
  TestGetDataCounts();
  TestInsideCalls();
  return TestSummary("test_d3d11_query_park");
}
