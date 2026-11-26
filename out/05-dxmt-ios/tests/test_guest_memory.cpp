// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 2: the guest-memory ledger and its lines (dxmt/src/util/util_guest_memory.hpp)
// and the per-resource cap's rule (dxmt/src/dxmt/dxmt_dynamic_policy.hpp GuestDiscardWaitSeq).
// The 2000-discard and 2000-update bounds and a failed allocation run on DXMT's own buffer and
// ring code in bench_dynamic_discard.cpp (CheckGuestWindow).

#include "test_common.hpp"

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
        "[dxmt-wow] guest memory: 1003 blocks, 2176.5 MB (maps 2112.0, staging 64.0, other 0.5)");
  CHECK(ReportLine(Snapshot{}) == "[dxmt-wow] guest memory: 0 blocks, 0.0 MB (maps 0.0, staging 0.0, other 0.0)");
  CHECK(FailureLine(0x210000, "a buffer") == "[dxmt-wow] guest allocation of 2162688 failed: a buffer");
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

} // namespace

int
main() {
  TestLines();
  TestLedger();
  TestCapRule();
  return TestSummary("test_guest_memory");
}
