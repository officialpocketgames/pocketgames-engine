// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 Part D: host tests for the pure rules of the Direct3D 9 frontend's
// iOS behaviour (dxmt/src/d3d9/d3d9_ios_policy.hpp): the switch parsing, C3's
// upload-pressure threshold, C4's recorded-work budget and reserve cap, C5,
// C6's batch split, C7's poll-run rule, C9's mode list, C13 and C15.
//
//   c++ -std=c++20 -I../dxmt/src/d3d9 test_d3d9_policy.cpp -o test_d3d9_policy

#include "test_common.hpp"

#include "d3d9_ios_policy.hpp"

#include <vector>

using namespace dxmt::d9ios;

namespace {

void
TestSwitches() {
  CHECK(ParseSwitch(nullptr, true));
  CHECK(!ParseSwitch(nullptr, false));
  CHECK(ParseSwitch("", true));   // empty: the default
  CHECK(!ParseSwitch("", false));
  CHECK(!ParseSwitch("0", true)); // "0": off
  CHECK(ParseSwitch("1", false)); // anything else: on
  CHECK(ParseSwitch("00", false));
  CHECK(ParseSwitch("no", false));

  CHECK_EQ(ParseCount(nullptr, 7), 7);
  CHECK_EQ(ParseCount("", 7), 7);
  CHECK_EQ(ParseCount("0", 7), 0);
  CHECK_EQ(ParseCount("128", 7), 128);
  CHECK_EQ(ParseCount("12x", 7), 7);
  CHECK_EQ(ParseCount("-1", 7), 7);
  CHECK_EQ(ParseCount("99999999999999999999999", 7), 7); // overflow
}

void
TestUploadPressure() {
  // C3: DXMT_D9_UPLOAD_COMMIT_MB, default 64, 0 never.
  CHECK_EQ(UploadCommitThresholdBytes(nullptr), 64ull << 20);
  CHECK_EQ(UploadCommitThresholdBytes(""), 64ull << 20);
  CHECK_EQ(UploadCommitThresholdBytes("16"), 16ull << 20);
  CHECK_EQ(UploadCommitThresholdBytes("0"), 0);
  CHECK_EQ(UploadCommitThresholdBytes("junk"), 64ull << 20);

  UploadPressure p(UploadCommitThresholdBytes("16"));
  CHECK(!p.due());
  p.noteStaged(10ull << 20);
  CHECK(!p.due());
  p.noteStaged(6ull << 20); // exactly the threshold
  CHECK(p.due());
  CHECK_EQ(p.previousCommitSeq(), 0); // no pressure commit yet: no wait
  p.onPressureCommit(41);
  CHECK(!p.due());
  CHECK_EQ(p.staged(), 0);
  CHECK_EQ(p.previousCommitSeq(), 41); // the next pressure commit waits for this one

  // Any commit restarts the count, and keeps the remembered pressure commit.
  p.noteStaged(15ull << 20);
  p.onCommit();
  CHECK_EQ(p.staged(), 0);
  CHECK_EQ(p.previousCommitSeq(), 41);
  p.noteStaged(17ull << 20);
  CHECK(p.due());

  // 0 means never.
  UploadPressure never(0);
  never.noteStaged(UINT64_MAX);
  never.noteStaged(1); // saturates instead of wrapping
  CHECK_EQ(never.staged(), (long long)UINT64_MAX);
  CHECK(!never.due());
}

void
TestRecordedWorkBudget() {
  // C4 (clarification 1): 16 MiB of used bytes after a batch;
  // MADEIRA_D3D9_BATCH_BUDGET=0 disables.
  CHECK(!RecordedWorkOverBudget((16ull << 20) - 1, true));
  CHECK(RecordedWorkOverBudget(16ull << 20, true));
  CHECK(RecordedWorkOverBudget(100ull << 20, true));
  CHECK(!RecordedWorkOverBudget(100ull << 20, false));

  // The reserve for the next batch: min(high-water, 64 KiB / element size),
  // no minimum; the whole high-water mark with the budget off.
  CHECK_EQ(PendingReserveCount(0, 64), 0);
  CHECK_EQ(PendingReserveCount(100, 64), 100);
  CHECK_EQ(PendingReserveCount(1000000, 64), (64 << 10) / 64);
  CHECK_EQ(PendingReserveCount(1000000, 1000), 65);          // 65536 / 1000
  CHECK_EQ(PendingReserveCount(1000000, 1 << 20), 0);        // bigger than 64 KiB: none
  CHECK_EQ(PendingReserveCount(1000000, 64, false), 1000000); // budget off: no cap

  // The tight copy before hand-off: > 64 KiB and under half full, or > 4 KiB
  // and under a quarter full.
  CHECK(PendingShouldShrink(100, 2048, 64));    // 128 KiB, 5% full
  CHECK(PendingShouldShrink(1023, 2048, 64));   // 128 KiB, just under half
  CHECK(!PendingShouldShrink(1024, 2048, 64));  // exactly half: keep
  CHECK(PendingShouldShrink(15, 64, 128));      // 8 KiB, under a quarter
  CHECK(!PendingShouldShrink(16, 64, 128));     // 8 KiB, a quarter: keep
  CHECK(!PendingShouldShrink(20, 64, 128));     // 8 KiB, 31%: keep
  CHECK(!PendingShouldShrink(0, 64, 64));       // exactly 4 KiB: keep
  CHECK(!PendingShouldShrink(0, 0, 64));        // nothing held

  std::vector<uint64_t> v;
  v.reserve(4096); // 32 KiB
  for (uint64_t i = 0; i < 10; i++)
    v.push_back(i * 7);
  ShrinkPendingVector(v);
  CHECK(v.capacity() < 4096);
  CHECK_EQ(v.size(), 10);
  for (uint64_t i = 0; i < v.size(); i++)
    CHECK_EQ(v[i], i * 7);
  std::vector<uint64_t> full(1000, 3); // 8000 bytes, full: kept as it is
  const auto *data = full.data();
  ShrinkPendingVector(full);
  CHECK(full.data() == data);
}

void
TestSceneBatch() {
  // C5: EndScene keeps fewer than 256 recorded operations.
  CHECK(!EndSceneDrains(true, 1));
  CHECK(!EndSceneDrains(true, 255));
  CHECK(EndSceneDrains(true, 256));
  CHECK(EndSceneDrains(true, 0)); // only clears: the normal path
  CHECK(EndSceneDrains(false, 1));
}

void
TestReadbackBatches() {
  // C6: batches of at most 16 MiB; an oversized entry alone.
  const uint64_t MiB = 1ull << 20;
  std::vector<uint64_t> sizes = {4 * MiB, 4 * MiB, 8 * MiB, 1 * MiB, 20 * MiB, 2 * MiB};
  size_t end = ReadbackBatchEnd(sizes.data(), sizes.size(), 0);
  CHECK_EQ(end, 3); // 4 + 4 + 8 = 16
  end = ReadbackBatchEnd(sizes.data(), sizes.size(), 3);
  CHECK_EQ(end, 4); // 1, then 20 doesn't fit
  end = ReadbackBatchEnd(sizes.data(), sizes.size(), 4);
  CHECK_EQ(end, 5); // 20 alone
  end = ReadbackBatchEnd(sizes.data(), sizes.size(), 5);
  CHECK_EQ(end, 6);
  CHECK_EQ(ReadbackBatchEnd(sizes.data(), sizes.size(), 6), 6);
}

void
TestQueryPolling() {
  // C7 (clarification 2), adaptive: not-ready polls whose starts are at most
  // 250 us apart form a run; the first two return at once, the third and later
  // park. The earlier poll's park counts in the gap.
  QueryPollState s;
  s.onIssueEnd();
  CHECK(!QueryPollShouldPark(s, 1000000, true)); // run 1
  CHECK(!QueryPollShouldPark(s, 1000050, true)); // 50 us later: run 2, still at once
  CHECK(QueryPollShouldPark(s, 1000100, true));  // run 3: park (up to 100 us)
  CHECK(QueryPollShouldPark(s, 1000220, true));  // 120 us incl. the park: run 4, park
  CHECK(!QueryPollShouldPark(s, 1000220 + 250001, true)); // > 250 us: new run at 1
  CHECK(!QueryPollShouldPark(s, 1250221 + 250000, true)); // exactly 250 us: run 2
  CHECK(QueryPollShouldPark(s, 1500221 + 1, true));        // run 3: park

  // A new Issue(END) starts over.
  s.onIssueEnd();
  CHECK(!QueryPollShouldPark(s, 2000000, true));
  CHECK(!QueryPollShouldPark(s, 2000001, true));
  CHECK(QueryPollShouldPark(s, 2000002, true));

  // Not adaptive: count every not-ready poll since Issue, whatever the spacing;
  // park once the count is more than 2.
  QueryPollState n;
  n.onIssueEnd();
  CHECK(!QueryPollShouldPark(n, 0, false));
  CHECK(!QueryPollShouldPark(n, 10000000000ull, false));
  CHECK(QueryPollShouldPark(n, 20000000000ull, false));
  CHECK(QueryPollShouldPark(n, 30000000000ull, false));

  // A clock that went backwards doesn't continue a run.
  QueryPollState b;
  b.onIssueEnd();
  CHECK(!QueryPollShouldPark(b, 5000, true));
  CHECK(!QueryPollShouldPark(b, 5100, true));
  CHECK(!QueryPollShouldPark(b, 4000, true)); // backwards: new run at 1
  CHECK(!QueryPollShouldPark(b, 4100, true));
  CHECK(QueryPollShouldPark(b, 4200, true));

  // The forward-progress submit: once per Issue(END).
  QueryPollState f;
  f.onIssueEnd();
  CHECK(QueryTakeFlush(f));
  CHECK(!QueryTakeFlush(f));
  CHECK(!QueryTakeFlush(f));
  f.onIssueEnd();
  CHECK(QueryTakeFlush(f));
}

struct Mode {
  unsigned w, h, hz;
};

void
TestAdapterModes() {
  // C9: every mode for X8R8G8B8 (22), R5G6B5 (23), X1R5G5B5 (24); none else.
  const std::vector<Mode> display = {{640, 480, 60}, {800, 600, 60}, {1024, 768, 60}, {1920, 1080, 120}};
  for (uint32_t fmt : {22u, 23u, 24u}) {
    auto list = AdapterModesForFormat(fmt, display);
    CHECK_EQ(list.size(), display.size());
    for (size_t i = 0; i < list.size() && i < display.size(); i++) {
      CHECK_EQ(list[i].w, display[i].w);
      CHECK_EQ(list[i].h, display[i].h);
      CHECK_EQ(list[i].hz, display[i].hz);
    }
  }
  for (uint32_t fmt : {0u, 21u, 25u, 35u /* A2R10G10B10 */, 0xFFFFFFFFu})
    CHECK_EQ(AdapterModesForFormat(fmt, display).size(), 0);
  CHECK_EQ(AdapterModesForFormat(22u, std::vector<Mode>{}).size(), 0);
}

void
TestAnisotropy() {
  // C13
  CHECK_EQ(ParseAnisotropyLimit(nullptr), 0);
  CHECK_EQ(ParseAnisotropyLimit("1"), 1);
  CHECK_EQ(ParseAnisotropyLimit("2"), 2);
  CHECK_EQ(ParseAnisotropyLimit("4"), 4);
  CHECK_EQ(ParseAnisotropyLimit("8"), 8);
  CHECK_EQ(ParseAnisotropyLimit("3"), 0);
  CHECK_EQ(ParseAnisotropyLimit("16"), 0);
  CHECK_EQ(CapAnisotropy(16, 4), 4);
  CHECK_EQ(CapAnisotropy(2, 4), 2);
  CHECK_EQ(CapAnisotropy(16, 0), 16);
}

void
TestMirrorBudget() {
  // C15: 256 KB per resource, 32 MB in all.
  MirrorCacheBudget b;
  CHECK(!b.tryKeep(0));
  CHECK(!b.tryKeep((256u << 10) + 1)); // too big for the cache
  CHECK(b.tryKeep(256u << 10));
  CHECK_EQ(b.used(), 256 << 10);
  unsigned kept = 1;
  while (b.tryKeep(256u << 10))
    kept++;
  CHECK_EQ(kept, 128); // 128 x 256 KB = 32 MB
  CHECK_EQ(b.used(), 32 << 20);
  CHECK(!b.tryKeep(1));
  b.release(256u << 10);
  CHECK(b.tryKeep(100));
  CHECK_EQ(b.used(), (32u << 20) - (256u << 10) + 100);
}

} // namespace

int
main() {
  TestSwitches();
  TestUploadPressure();
  TestRecordedWorkBudget();
  TestSceneBatch();
  TestReadbackBatches();
  TestQueryPolling();
  TestAdapterModes();
  TestAnisotropy();
  TestMirrorBudget();
  std::printf("test_d3d9_policy: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
