// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 10: host tests of three of steamac's DXMT patches as this tree carries
// them, against DXMT's own code with Metal faked (MTLDevice_newBuffer hands out heap memory,
// the residency-set calls are recorded):
//
// - 0002, StagingBufferBlockAllocator (dxmt_ring_bump_allocator.hpp): a placed block writes
//   where newBuffer says the contents are, also when the unix side backs the buffer with other
//   memory than the placed one, and frees the placed memory (after releasing the buffer, D6).
// - 0004, a query that ends in a submission without visibility samples
//   (dxmt_occlusion_query.hpp, as ArgumentEncodingContext::flushCommands uses it under H1):
//   QueriesEndingAt picks the queries the empty readback settles; destroying the readbacks in
//   submission order completes the query with the samples of the earlier submission.
// - 0023, ResidencySetTracker and Allocation (dxmt_residency.hpp, dxmt_allocation.{hpp,cpp}):
//   an allocation is added once, a commit happens only when something was added, the last
//   decRef takes the object out and commits at once, and the set is released with the last
//   reference to the tracker. Which path a queue takes (iOS 18 and later only) is a device check.

#include "test_common.hpp"

#include "dxmt_allocation.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_residency.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "wsi_platform.hpp"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

/* ---- fake Metal ------------------------------------------------------------ */

namespace {

struct FakeMetal {
  obj_handle_t next_handle = 0x1000;
  std::map<obj_handle_t, void *> owned_contents; // memory Metal allocated (newBufferWithLength:)
  void *remote_contents = nullptr;                // when set, newBuffer reports this memory instead
  std::vector<std::string> events;
  std::vector<obj_handle_t> residency_added, residency_removed;
  unsigned residency_commits = 0;
  std::map<obj_handle_t, unsigned> released;
};

FakeMetal g_metal;

} // namespace

extern "C" {

obj_handle_t
MTLDevice_newBuffer(obj_handle_t, struct WMTBufferInfo *info) {
  obj_handle_t h = g_metal.next_handle++;
  if (g_metal.remote_contents) {
    // a unix side that backs the buffer with memory of its own (as a remoting one does)
    info->memory.set(g_metal.remote_contents);
  } else if (!info->memory.get()) {
    void *mem = std::calloc(1, info->length);
    g_metal.owned_contents[h] = mem;
    info->memory.set(mem);
  }
  info->gpu_address = 0x100000000ull + uint64_t(h) * 0x10000ull;
  g_metal.events.push_back("newBuffer");
  return h;
}

void
NSObject_retain(obj_handle_t) {}

void
NSObject_release(obj_handle_t obj) {
  g_metal.released[obj]++;
  g_metal.events.push_back("release");
  auto it = g_metal.owned_contents.find(obj);
  if (it != g_metal.owned_contents.end()) {
    std::free(it->second);
    g_metal.owned_contents.erase(it);
  }
}

void
MTLResidencySet_addAllocations(obj_handle_t, const obj_handle_t *allocations, uint64_t count) {
  for (uint64_t i = 0; i < count; i++)
    g_metal.residency_added.push_back(allocations[i]);
}

void
MTLResidencySet_removeAllocations(obj_handle_t, const obj_handle_t *allocations, uint64_t count) {
  for (uint64_t i = 0; i < count; i++)
    g_metal.residency_removed.push_back(allocations[i]);
}

void
MTLResidencySet_commit(obj_handle_t) {
  g_metal.residency_commits++;
}

} // extern "C"

namespace dxmt {
namespace wsi {
void *
aligned_malloc(size_t size, size_t alignment) {
  g_metal.events.push_back("malloc");
  return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}
void
aligned_free(void *ptr) {
  g_metal.events.push_back("free");
  std::free(ptr);
}
} // namespace wsi
} // namespace dxmt

using namespace dxmt;

namespace {

/* ---- 0002 ---------------------------------------------------------------- */

void
TestStagingBlockMapsReturnedMemory() {
  WMT::Device device{0x10};
  StagingBufferBlockAllocator allocator(device, WMTResourceStorageModeShared, true);

  {
    // the usual unix side: the buffer is made on the placed memory itself
    g_metal.events.clear();
    auto block = allocator.allocate(4096);
    CHECK(block.placed_address != nullptr);
    CHECK(block.mapped_address == block.placed_address);
    CHECK(block.buffer.handle != 0);
  }
  // released before its memory is freed (D6)
  CHECK_EQ(g_metal.events.size(), 4);
  CHECK(g_metal.events[0] == "malloc" && g_metal.events[1] == "newBuffer");
  CHECK(g_metal.events[2] == "release" && g_metal.events[3] == "free");

  {
    // a unix side that reports other memory: CPU writes must go there
    static char remote[16384];
    g_metal.remote_contents = remote;
    g_metal.events.clear();
    auto block = allocator.allocate(4096);
    CHECK(block.mapped_address == remote);
    CHECK(block.placed_address != nullptr && block.placed_address != remote);
    std::memset(block.mapped_address, 0x5a, 16);
    CHECK_EQ(remote[15], 0x5a);

    // moving the block moves both pointers
    auto moved = std::move(block);
    CHECK(moved.mapped_address == remote);
    CHECK(block.mapped_address == nullptr && block.placed_address == nullptr);
    g_metal.remote_contents = nullptr;
  }
  // the placed memory is what is freed, once (the moved-from block frees nothing)
  unsigned frees = 0;
  for (auto &e : g_metal.events)
    frees += e == "free";
  CHECK_EQ(frees, 1);

  // not placed: no CPU address, nothing allocated or freed by the block
  StagingBufferBlockAllocator unplaced(device, WMTResourceStorageModePrivate, false);
  g_metal.events.clear();
  {
    auto block = unplaced.allocate(4096);
    if (!kI386Build) {
      CHECK(block.placed_address == nullptr);
      CHECK(block.mapped_address == nullptr);
    }
  }
  if (!kI386Build) {
    for (auto &e : g_metal.events)
      CHECK(e != "malloc" && e != "free");
  }
}

/* ---- 0004 ---------------------------------------------------------------- */

void
TestQueryEndingInEmptySubmission() {
  WMT::Device device{0x10};
  Rc<VisibilityResultQuery> query = new VisibilityResultQuery();
  Rc<VisibilityResultQuery> other = new VisibilityResultQuery();
  std::vector<Rc<VisibilityResultQuery>> pending;

  // submission 1: the query begins at sample 2 of 4; `other` begins and ends in it
  query->begin(1, 2);
  pending.push_back(query);
  other->begin(1, 0);
  other->end(1, 2);
  pending.push_back(other);
  // submission 2 records no visibility samples; the query ends there
  query->end(2, 0);

  uint64_t value = ~0ull;
  CHECK(!query->getValue(&value));

  CHECK_EQ(QueriesEndingAt(pending, 1).size(), 1);
  CHECK(QueriesEndingAt(pending, 1)[0].ptr() == other.ptr());
  auto ending2 = QueriesEndingAt(pending, 2);
  CHECK_EQ(ending2.size(), 1);
  CHECK(ending2[0].ptr() == query.ptr());
  CHECK(QueriesEndingAt(pending, 3).empty());

  auto readback1 = std::make_unique<VisibilityResultReadback>(device, 1, 4, pending);
  auto *samples = (uint64_t *)readback1->visibility_result_heap_info.memory.get();
  samples[0] = 1;
  samples[1] = 2;
  samples[2] = 30; // the GPU's counts
  samples[3] = 400;
  // flushCommands drops the queries that ended in submission 1
  std::erase_if(pending, [](auto &q) { return q->queryEndAt() == 1; });
  // H1 (on in every build since clarification 10): the empty readback of submission 2
  auto readback2 = std::make_unique<VisibilityResultReadback>(2, std::move(ending2));
  CHECK(readback2->visibility_result_heap == nullptr); // no buffer for an empty submission

  // the finish thread destroys them in submission order
  readback1.reset();
  CHECK(other->getValue(&value));
  CHECK_EQ(value, 3);               // samples 0 and 1
  CHECK(!query->getValue(&value)); // not complete before submission 2 retires
  readback2.reset();
  CHECK(query->getValue(&value));
  CHECK_EQ(value, 430); // samples 2 and 3 of submission 1, none in submission 2
}

/* ---- 0023 ---------------------------------------------------------------- */

struct TestAllocation : Allocation {
  explicit TestAllocation(bool &destroyed) : destroyed_(destroyed) {}
  ~TestAllocation() { destroyed_ = true; }
  bool &destroyed_;
};

void
TestResidencySetTracker() {
  g_metal.residency_added.clear();
  g_metal.residency_removed.clear();
  g_metal.residency_commits = 0;
  g_metal.released.clear();

  const obj_handle_t set_handle = 0x9000;
  Rc<ResidencySetTracker> tracker = new ResidencySetTracker(WMT::Reference<WMT::ResidencySet>(set_handle));

  bool destroyed_a = false, destroyed_b = false;
  auto *a = new TestAllocation(destroyed_a);
  auto *b = new TestAllocation(destroyed_b);
  a->incRef();
  b->incRef();

  tracker->commit(); // nothing added yet
  CHECK_EQ(g_metal.residency_commits, 0);

  a->useResidencySet(tracker.ptr(), WMT::Object{0x111});
  a->useResidencySet(tracker.ptr(), WMT::Object{0x111}); // a second use adds nothing
  CHECK_EQ(g_metal.residency_added.size(), 1);
  CHECK_EQ(g_metal.residency_added[0], 0x111);
  tracker->commit(); // before the command buffer that uses it
  CHECK_EQ(g_metal.residency_commits, 1);
  tracker->commit(); // nothing new
  CHECK_EQ(g_metal.residency_commits, 1);

  a->incRef(); // a chunk still holds it
  a->decRef();
  CHECK(!destroyed_a);
  CHECK(g_metal.residency_removed.empty());
  a->decRef(); // the last reference: out of the set, committed at once
  CHECK(destroyed_a);
  CHECK_EQ(g_metal.residency_removed.size(), 1);
  CHECK_EQ(g_metal.residency_removed[0], 0x111);
  CHECK_EQ(g_metal.residency_commits, 2);

  // never used with the set: freeing it leaves the set alone
  b->decRef();
  CHECK(destroyed_b);
  CHECK_EQ(g_metal.residency_removed.size(), 1);
  CHECK_EQ(g_metal.residency_commits, 2);

  // an allocation keeps the tracker (and the Metal set) alive after the queue let go
  bool destroyed_c = false;
  auto *c = new TestAllocation(destroyed_c);
  c->incRef();
  c->useResidencySet(tracker.ptr(), WMT::Object{0x222});
  tracker = nullptr;
  CHECK_EQ(g_metal.released[set_handle], 0);
  c->decRef();
  CHECK(destroyed_c);
  CHECK_EQ(g_metal.residency_removed.back(), 0x222);
  CHECK_EQ(g_metal.released[set_handle], 1);
}

} // namespace

int
main() {
  TestStagingBlockMapsReturnedMemory();
  TestQueryEndingInEmptySubmission();
  TestResidencySetTracker();
  return TestSummary("test_core_fixes");
}
