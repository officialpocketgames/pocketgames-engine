// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 57: PG_DXMT_SERIALIZE (dxmt/src/dxmt/dxmt_serialize.hpp). The level's
// parsing and what each level turns on; the encoder optimiser's loop with a relation shaped
// like checkEncoderRelation (it merges two matching render passes across an unrelated blit
// unless the level keeps the order); the waits each level asks for, on a fake queue; the line
// every 600 presents. Whether the GPU then stops overlapping is the device's check.

#include "test_common.hpp"

#include "dxmt_serialize.hpp"

#include <string>
#include <vector>

namespace {

using namespace dxmt;

void
TestLevels() {
  CHECK_EQ(SerializeSettingFromValue(nullptr).level, 0u);
  CHECK(!SerializeSettingFromValue(nullptr).ignored);
  CHECK_EQ(SerializeSettingFromValue("").level, 0u);
  CHECK(!SerializeSettingFromValue("").ignored);
  CHECK_EQ(SerializeSettingFromValue("0").level, 0u);
  CHECK(!SerializeSettingFromValue("0").ignored);
  for (unsigned n = 1; n <= 4; n++) {
    char value[2] = {char('0' + n), 0};
    CHECK_EQ(SerializeSettingFromValue(value).level, n);
    CHECK(!SerializeSettingFromValue(value).ignored);
  }
  for (const char *bad : {"5", "9", "12", "01", "x", "-1", "1 "}) {
    CHECK_EQ(SerializeSettingFromValue(bad).level, 0u);
    CHECK(SerializeSettingFromValue(bad).ignored);
  }
  CHECK(SerializeLevelLine(3) == "dxmt: serialize level 3 (test switch)");

  // each level includes the ones below it
  CHECK(!SerializeKeepsEncoderOrder(0) && !SerializeWaitsAfterPresent(0));
  CHECK(!SerializeWaitsBeforeCpuWrite(0) && !SerializeFencesEncoders(0));
  CHECK(SerializeKeepsEncoderOrder(1) && !SerializeWaitsAfterPresent(1));
  CHECK(SerializeKeepsEncoderOrder(2) && SerializeWaitsAfterPresent(2) && !SerializeWaitsBeforeCpuWrite(2));
  CHECK(SerializeWaitsAfterPresent(3) && SerializeWaitsBeforeCpuWrite(3) && !SerializeFencesEncoders(3));
  CHECK(SerializeKeepsEncoderOrder(4) && SerializeWaitsAfterPresent(4) && SerializeWaitsBeforeCpuWrite(4) &&
        SerializeFencesEncoders(4));
}

/* ---- the optimiser ---- */

enum class Kind { Null, Render, Blit, Clear };

struct FakeEncoder {
  Kind kind;
  int target;  // a render pass's or clear's attachment
  int touches; // a blit's buffer
  int merged = 0;
};

/* Shaped like checkEncoderRelation: empty encoders are passed over; from level 1 on every pair
   synchronises before anything else; a clear followed by a render pass on its target, or two
   render passes on the same target, merge (the earlier one empties into the later); a blit that
   touches nothing the pass uses lets the search go on past it. */
unsigned
OptimizedCount(std::vector<FakeEncoder> list, unsigned level) {
  std::vector<FakeEncoder *> encoders;
  for (auto &e : list)
    encoders.push_back(&e);
  OptimizeEncoderList(
      encoders.data(), unsigned(encoders.size()), 64,
      [level](FakeEncoder *former, FakeEncoder *latter) {
        if (SerializeKeepsEncoderOrder(level))
          return true;
        if ((former->kind == Kind::Render || former->kind == Kind::Clear) && latter->kind == Kind::Render &&
            former->target == latter->target) {
          latter->merged += 1 + former->merged;
          former->kind = Kind::Null;
          return true;
        }
        if (latter->kind == Kind::Blit && latter->touches != former->target)
          return false; // independent: look further
        return true;
      },
      [](FakeEncoder *e) { return e->kind == Kind::Null; }
  );
  unsigned passes = 0;
  for (auto &e : list)
    passes += e.kind != Kind::Null;
  return passes;
}

void
TestOptimizer() {
  // two matching render passes with an unrelated blit between them
  std::vector<FakeEncoder> list = {{Kind::Render, 7, 0}, {Kind::Blit, 0, 3}, {Kind::Render, 7, 0}};
  CHECK_EQ(OptimizedCount(list, 0), 2u); // merged across the blit
  for (unsigned level = 1; level <= 4; level++)
    CHECK_EQ(OptimizedCount(list, level), 3u); // kept: three passes, in order

  // a clear before its render pass: merged at level 0, its own pass from level 1
  std::vector<FakeEncoder> clear = {{Kind::Clear, 5, 0}, {Kind::Render, 5, 0}};
  CHECK_EQ(OptimizedCount(clear, 0), 1u);
  CHECK_EQ(OptimizedCount(clear, 1), 2u);

  // a blit that writes what the pass uses stops the search at level 0 too
  std::vector<FakeEncoder> dependent = {{Kind::Render, 7, 0}, {Kind::Blit, 0, 7}, {Kind::Render, 7, 0}};
  CHECK_EQ(OptimizedCount(dependent, 0), 3u);

  // one encoder, none: nothing to do
  CHECK_EQ(OptimizedCount({{Kind::Render, 1, 0}}, 0), 1u);
  CHECK_EQ(OptimizedCount({}, 2), 0u);
}

/* ---- the waits ---- */

struct FakeQueue {
  uint64_t current = 10;  // the chunk being recorded
  uint64_t coherent = 6;  // the GPU has finished chunks up to here
  std::vector<uint64_t> waited;
  unsigned commits = 0;

  uint64_t
  CurrentSeqId() {
    return current;
  }
  uint64_t
  CoherentSeqId() {
    return coherent;
  }
  void
  WaitCPUFence(uint64_t seq) {
    waited.push_back(seq);
    coherent = seq; // the GPU gets there
  }
};

void
TestWaits() {
  for (unsigned level = 0; level <= 4; level++) {
    FakeQueue queue;
    SerializeStats stats;
    // after a present: the chunk just committed (9) at level 2 and above
    bool waited = SerializeAfterPresent(queue, stats, level);
    CHECK(waited == (level >= 2));
    CHECK_EQ(queue.waited.size(), size_t(level >= 2 ? 1 : 0));
    if (level >= 2)
      CHECK_EQ(queue.waited[0], 9u);

    // before a CPU write: commit the chunk being recorded, then wait for it, at 3 and above
    FakeQueue writes;
    writes.coherent = 9;
    bool held = SerializeBeforeCpuWrite(writes, stats, level, [&] {
      writes.commits++;
      writes.current++; // 10 committed, 11 now recorded
    });
    CHECK(held == (level >= 3));
    CHECK_EQ(writes.commits, level >= 3 ? 1u : 0u);
    if (level >= 3) {
      CHECK_EQ(writes.waited.size(), 1u);
      CHECK(writes.waited.size() == 1 && writes.waited[0] == 10u);
    } else {
      CHECK(writes.waited.empty());
    }
    CHECK_EQ(stats.waits(), uint64_t(level >= 3 ? 2 : (level == 2 ? 1 : 0)));
  }

  // nothing to wait for when the GPU is already there: no wait counted
  FakeQueue idle;
  idle.coherent = 9;
  SerializeStats stats;
  CHECK(!SerializeAfterPresent(idle, stats, 4));
  CHECK(idle.waited.empty());
  CHECK_EQ(stats.waits(), 0u);
  // before the first chunk was committed
  FakeQueue fresh;
  fresh.current = 0;
  fresh.coherent = 0;
  CHECK(!SerializeAfterPresent(fresh, stats, 4));
}

void
TestReport() {
  SerializeStats stats;
  stats.addWait(1500000);
  stats.addWait(2600000);
  std::string line;
  for (unsigned i = 1; i < 600; i++)
    CHECK(!stats.present(line));
  CHECK(stats.present(line));
  CHECK(line == "dxmt: serialize: 2 waits, 4 ms waited in the last 600 presents");
  // the next 600 start from zero
  for (unsigned i = 1; i < 600; i++)
    CHECK(!stats.present(line));
  CHECK(stats.present(line));
  CHECK(line == "dxmt: serialize: 0 waits, 0 ms waited in the last 600 presents");
}

} // namespace

int
main() {
  TestLevels();
  TestOptimizer();
  TestWaits();
  TestReport();
  return TestSummary("test_serialize");
}
