// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 25: an indirect dispatch's records (dxmt/src/dxmt/dxmt_indirect.hpp):
// a memory barrier over buffers and textures, then the dispatch, linked as the compute encoder
// links them (a fake with ArgumentEncodingContext::encodeComputeCommand's chaining).

#include "test_common.hpp"

#define WINEMETAL_API extern "C"
#include "winemetal.h"

#include "dxmt_indirect.hpp"

#include <cstring>
#include <vector>

namespace {

/* Appends each record to a chain, like ArgumentEncodingContext::encodeComputeCommand: storage
   that isn't zeroed (filled with 0xcd here), the previous record's `next` set to it. */
struct FakeComputeEncoder {
  wmtcmd_compute_nop head;
  wmtcmd_base *tail;
  std::vector<std::vector<unsigned char>> storage;

  FakeComputeEncoder() {
    std::memset(&head, 0, sizeof(head));
    head.type = WMTComputeCommandNop;
    head.next.set(nullptr);
    tail = (wmtcmd_base *)&head;
  }

  template <typename T>
  T &
  encodeComputeCommand() {
    storage.emplace_back(sizeof(T), (unsigned char)0xcd);
    auto record = (T *)storage.back().data();
    tail->next.set(record);
    tail = (wmtcmd_base *)record;
    record->next.set(nullptr);
    return *record;
  }
};

void
TestBarrierFirst() {
  FakeComputeEncoder enc;
  auto &dispatch = dxmt::EncodeIndirectDispatch(enc, 0x4242, 48);

  auto first = (wmtcmd_base *)enc.head.next.get();
  CHECK(first != nullptr);
  CHECK_EQ(first->type, WMTComputeCommandMemoryBarrier);
  auto barrier = (wmtcmd_compute_memory_barrier *)first;
  CHECK_EQ(barrier->scope, WMTBarrierScopeBuffers | WMTBarrierScopeTextures);
  for (auto byte : barrier->padding)
    CHECK_EQ(byte, 0);

  auto second = (wmtcmd_base *)first->next.get();
  CHECK(second == (wmtcmd_base *)&dispatch);
  CHECK_EQ(second->type, WMTComputeCommandDispatchIndirect);
  CHECK_EQ(dispatch.indirect_args_buffer, 0x4242);
  CHECK_EQ(dispatch.indirect_args_offset, 48);
  CHECK(second->next.get() == nullptr);
  CHECK_EQ(enc.storage.size(), 2);
}

void
TestTwoIndirectDispatches() {
  // each indirect dispatch gets its own barrier
  FakeComputeEncoder enc;
  dxmt::EncodeIndirectDispatch(enc, 1, 0);
  dxmt::EncodeIndirectDispatch(enc, 2, 12);
  std::vector<uint16_t> types;
  for (auto r = (wmtcmd_base *)enc.head.next.get(); r; r = (wmtcmd_base *)r->next.get())
    types.push_back(r->type);
  CHECK_EQ(types.size(), 4);
  CHECK_EQ(types[0], WMTComputeCommandMemoryBarrier);
  CHECK_EQ(types[1], WMTComputeCommandDispatchIndirect);
  CHECK_EQ(types[2], WMTComputeCommandMemoryBarrier);
  CHECK_EQ(types[3], WMTComputeCommandDispatchIndirect);
}

} // namespace

int
main() {
  TestBarrierFirst();
  TestTwoIndirectDispatches();
  return TestSummary("test_indirect_dispatch");
}
