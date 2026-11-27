// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 2: the guest-memory ledger and its lines (dxmt/src/util/util_guest_memory.hpp)
// and the per-resource cap's rule (dxmt/src/dxmt/dxmt_dynamic_policy.hpp GuestDiscardWaitSeq).
// The 2000-discard and 2000-update bounds and a failed allocation run on DXMT's own buffer and
// ring code in bench_dynamic_discard.cpp (CheckGuestWindow).
// Spec 06 round 3 (item 2): the commands use, the ring and command blocks by size, the line each
// 256 MB; the command blocks over 1000 frames are in bench_dynamic_discard.cpp (CheckCommandBlocks).
// Spec 06 round 4 (item 4): the command lists and the immediate context in the line; the backlog's
// rule and its first line; the wait's rule at the 1 GB mark and the chunk it waits for; the
// FinishCommandList line. The 2,000 lists and the wait on DXMT's ring code are in
// bench_dynamic_discard.cpp (CheckDeferredListBacklog).

#include "test_common.hpp"

#include "dxmt_cmdlist_backlog.hpp"
#include "dxmt_dynamic_policy.hpp"
#include "util_guest_memory.hpp"

#include <string>

namespace {

using namespace dxmt;
using namespace dxmt::guest_memory;

constexpr uint64_t kSecond = 1000000000ull;
constexpr uint64_t MB = 1024 * 1024;

void
TestLines() {
  Snapshot s;
  s.blocks = 1003;
  s.bytes[unsigned(Use::Maps)] = 2048 * MB + 64 * MB;
  s.bytes[unsigned(Use::Staging)] = 64 * MB;
  s.bytes[unsigned(Use::Other)] = MB / 2;
  CHECK(ReportLine(s) ==
        "[dxmt-wow] guest memory: 1003 blocks, 2176.5 MB (maps 2112.0, staging 64.0, commands 0.0, other 0.5)");
  CHECK(ReportLine(Snapshot{}) ==
        "[dxmt-wow] guest memory: 0 blocks, 0.0 MB (maps 0.0, staging 0.0, commands 0.0, other 0.0)");
  CHECK(FailureLine(0x210000, "a buffer") == "[dxmt-wow] guest allocation of 2162688 failed: a buffer");
  // spec 06 round 3: commands, and the live ring and command blocks by size, largest first
  Snapshot r3;
  r3.blocks = 975;
  r3.bytes[unsigned(Use::Staging)] = 4 * 8 * MB;
  r3.bytes[unsigned(Use::Commands)] = 970 * 2 * MB + 2 * MB;
  r3.sizes[0] = {2 * MB, 970};
  r3.sizes[1] = {8 * MB, 4};
  r3.sizes[2] = {MB, 2};
  r3.sizes[3] = {4096, 0}; // none live: left out
  CHECK(ReportLine(r3) == "[dxmt-wow] guest memory: 975 blocks, 1974.0 MB (maps 0.0, staging 32.0, commands 1942.0, "
                          "other 0.0); ring and command blocks: 8.0 MB x 4, 2.0 MB x 970, 1.0 MB x 2");
}

/* spec 06 round 3: the size table and the line each 256 MB */
void
TestRound3() {
  Ledger l;
  for (unsigned i = 0; i < 970; i++)
    l.add(Use::Commands, 2 * MB);
  l.add(Use::Staging, 8 * MB);
  l.add(Use::Commands, MB);
  l.add(Use::Maps, 2 * MB); // maps aren't ring or command blocks
  l.remove(Use::Commands, 2 * MB);
  Snapshot s = l.snapshot();
  uint64_t two = 0, eight = 0, one = 0, other = 0;
  for (const SizeCount &c : s.sizes) {
    if (!c.count)
      continue;
    if (c.size == 2 * MB)
      two = c.count;
    else if (c.size == 8 * MB)
      eight = c.count;
    else if (c.size == MB)
      one = c.count;
    else
      other += c.count;
  }
  CHECK_EQ(two, 969u);
  CHECK_EQ(eight, 1u);
  CHECK_EQ(one, 1u);
  CHECK_EQ(other, 0u);
  CHECK_EQ(l.totalBytes(), 969 * 2 * MB + 8 * MB + MB + 2 * MB);
  // a slot whose blocks all went is free for another size
  l.remove(Use::Commands, MB);
  l.add(Use::Commands, 4 * MB);
  bool four = false, gone_one = true;
  for (const SizeCount &c : l.snapshot().sizes) {
    if (c.count && c.size == 4 * MB)
      four = true;
    if (c.count && c.size == MB)
      gone_one = false;
  }
  CHECK(four && gone_one);
  // more sizes than slots: the rest go uncounted, and their removal changes nothing
  Ledger crowded;
  for (unsigned i = 0; i < kSizeSlots + 3; i++)
    crowded.add(Use::Staging, (i + 1) * MB);
  unsigned slots = 0;
  for (const SizeCount &c : crowded.snapshot().sizes)
    slots += c.count ? 1 : 0;
  CHECK_EQ(slots, kSizeSlots);
  crowded.remove(Use::Staging, (kSizeSlots + 2) * MB);
  slots = 0;
  for (const SizeCount &c : crowded.snapshot().sizes)
    slots += c.count ? 1 : 0;
  CHECK_EQ(slots, kSizeSlots);

  // the line each time the total passes another 256 MB, once each
  Ledger steps;
  CHECK(!steps.passedStep(255 * MB));
  CHECK(steps.passedStep(256 * MB));
  CHECK(!steps.passedStep(300 * MB));
  CHECK(!steps.passedStep(256 * MB));
  CHECK(steps.passedStep(600 * MB)); // past 512: the next is 768
  CHECK(!steps.passedStep(700 * MB));
  CHECK(steps.passedStep(768 * MB));
  CHECK(!steps.passedStep(100 * MB)); // going down doesn't start it again
  CHECK(steps.passedStep(2000 * MB));
}

void
TestLedger() {
  Ledger l;
  l.add(Use::Maps, 2 * MB);
  l.add(Use::Maps, 2 * MB);
  l.add(Use::Staging, 8 * MB);
  l.remove(Use::Maps, 2 * MB);
  Snapshot s = l.snapshot();
  CHECK_EQ(s.blocks, 2u);
  CHECK_EQ(s.bytes[unsigned(Use::Maps)], 2 * MB);
  CHECK_EQ(s.bytes[unsigned(Use::Staging)], 8 * MB);
  CHECK_EQ(s.bytes[unsigned(Use::Other)], 0u);
  // the report: the first call starts the period, then once per 10 s
  CHECK(!l.reportDue(5 * kSecond));
  CHECK(!l.reportDue(14 * kSecond));
  CHECK(l.reportDue(15 * kSecond));
  CHECK(!l.reportDue(15 * kSecond));
  CHECK(!l.reportDue(24 * kSecond));
  CHECK(l.reportDue(25 * kSecond));
  // the failure line once
  CHECK(l.takeFailureLine());
  CHECK(!l.takeFailureLine());
}

void
TestCapRule() {
  // off (the 64-bit build): never waits
  CHECK_EQ(GuestDiscardWaitSeq(false, 10, 5, 0), 0u);
  // below the cap: no wait
  CHECK_EQ(GuestDiscardWaitSeq(true, kGuestDynamicAllocations - 1, 7, 0), 0u);
  // at the cap with the oldest still on the GPU: wait for its chunk
  CHECK_EQ(GuestDiscardWaitSeq(true, kGuestDynamicAllocations, 7, 6), 7u);
  CHECK_EQ(GuestDiscardWaitSeq(true, kGuestDynamicAllocations + 4, 7, 2), 7u);
  // at the cap, the oldest finished: it is reused, no wait
  CHECK_EQ(GuestDiscardWaitSeq(true, kGuestDynamicAllocations, 7, 7), 0u);
  // nothing queued (only the current one): nothing to wait for
  CHECK_EQ(GuestDiscardWaitSeq(true, kGuestDynamicAllocations, 0, 0), 0u);
  CHECK_EQ(kGuestDynamicAllocations, 3u);
}

/* Spec 06 round 4 (item 4) */
void
TestRound4() {
  using guest_memory::ListCount;
  constexpr uint64_t kMB = 1ull << 20;

  // the line: the lists, the presents and the commits, once there is any
  guest_memory::Snapshot s;
  s.blocks = 2;
  s.bytes[unsigned(guest_memory::Use::Commands)] = 4 * kMB;
  s.sizes[0] = {2 * kMB, 2};
  CHECK(guest_memory::ReportLine(s) == "[dxmt-wow] guest memory: 2 blocks, 4.0 MB (maps 0.0, staging 0.0, commands 4.0, "
                                       "other 0.0); ring and command blocks: 2.0 MB x 2");
  s.lists[unsigned(ListCount::Made)] = 40;
  s.lists[unsigned(ListCount::Pooled)] = 25;
  s.lists[unsigned(ListCount::Waiting)] = 9;
  s.lists[unsigned(ListCount::Presents)] = 0;
  s.lists[unsigned(ListCount::Commits)] = 211;
  CHECK(guest_memory::ReportLine(s) ==
        "[dxmt-wow] guest memory: 2 blocks, 4.0 MB (maps 0.0, staging 0.0, commands 4.0, other 0.0); ring and command "
        "blocks: 2.0 MB x 2; command lists: 40 made, 15 alive, 9 waiting in chunks, 25 pooled; 0 presents, 211 "
        "commits");
  guest_memory::Snapshot presented;
  presented.lists[unsigned(ListCount::Presents)] = 3;
  presented.lists[unsigned(ListCount::Commits)] = 3;
  CHECK(guest_memory::ReportLine(presented) ==
        "[dxmt-wow] guest memory: 0 blocks, 0.0 MB (maps 0.0, staging 0.0, commands 0.0, other 0.0); command lists: 0 "
        "made, 0 alive, 0 waiting in chunks, 0 pooled; 3 presents, 3 commits");

  // the ledger's counts and one use's bytes
  guest_memory::Ledger ledger;
  ledger.countList(ListCount::Made, 3);
  ledger.countList(ListCount::Pooled, 2);
  ledger.countList(ListCount::Pooled, -1);
  ledger.countList(ListCount::Waiting, 1);
  ledger.add(guest_memory::Use::Commands, 64 * 1024);
  ledger.add(guest_memory::Use::Maps, 4096);
  guest_memory::Snapshot counted = ledger.snapshot();
  CHECK_EQ(counted.lists[unsigned(ListCount::Made)], 3u);
  CHECK_EQ(counted.lists[unsigned(ListCount::Pooled)], 1u);
  CHECK_EQ(counted.lists[unsigned(ListCount::Waiting)], 1u);
  CHECK_EQ(ledger.bytesOf(guest_memory::Use::Commands), 64u * 1024u);
  CHECK_EQ(ledger.bytesOf(guest_memory::Use::Maps), 4096u);

  // the backlog: more than 8 lists, or more than 16 MB, since the last commit
  CommandListBacklog backlog;
  for (unsigned i = 1; i <= kBacklogLists; i++)
    CHECK(!backlog.executed(64 * 1024));
  CHECK(backlog.executed(64 * 1024)); // the 9th
  CHECK_EQ(backlog.lists(), 9u);
  CHECK(backlog.takeFirstLine() == "[d3d11] command list backlog: 9 lists, 0.6 MB since the last commit; committing");
  CHECK(backlog.takeFirstLine().empty()); // the first only
  backlog.committed();
  CHECK_EQ(backlog.lists(), 0u);
  CHECK_EQ(backlog.bytes(), 0u);
  CHECK(!backlog.executed(16 * kMB)); // 16 MB exactly isn't past it
  CHECK(backlog.executed(1));         // one byte more is
  backlog.committed();
  CHECK(backlog.executed(40 * kMB)); // one large list alone
  CHECK_EQ(kBacklogBytes, 16 * kMB);

  // the wait: a 32-bit process past 1 GB of command memory only
  CHECK_EQ(kCommandWaitBytes, 1024 * kMB);
  CHECK(!CommandMemoryWaitDue(true, 1024 * kMB));
  CHECK(CommandMemoryWaitDue(true, 1024 * kMB + 1));
  CHECK(!CommandMemoryWaitDue(false, 3824 * kMB)); // 64-bit: never
  CHECK(CommandMemoryWaitDue(true, 33 * kMB, 32 * kMB));
  // the chunk waited for: the oldest committed and unfinished; none when all have finished
  CHECK_EQ(OldestUnfinishedChunk(10, 14), 11u);
  CHECK_EQ(OldestUnfinishedChunk(13, 14), 0u); // only the chunk being recorded
  CHECK_EQ(OldestUnfinishedChunk(0, 1), 0u);
  CHECK(CommandWaitLine(1025 * kMB) ==
        "[d3d11] command memory 1025.0 MB, past 1024 MB: FinishCommandList waits for the oldest committed chunk");
  CHECK(FinishOutOfMemoryLine(3824 * kMB) ==
        "[d3d11] FinishCommandList -> E_OUTOFMEMORY: a command block couldn't be had (commands 3824.0 MB)");
}

} // namespace

int
main() {
  TestLines();
  TestLedger();
  TestRound3();
  TestCapRule();
  TestRound4();
  return TestSummary("test_guest_memory");
}
