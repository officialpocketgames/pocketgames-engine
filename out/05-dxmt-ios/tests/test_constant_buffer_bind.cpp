// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 19: the range a *SetConstantBuffers / *SetConstantBuffers1 call binds
// (dxmt/src/d3d11/d3d11_constant_buffer_bind.hpp), what a rebind of the same buffer changes,
// and the once-per-process "constant buffer offsets in use" rule.

#include "test_common.hpp"

#include "d3d11_constant_buffer_bind.hpp"

#include <atomic>

namespace {

using dxmt::ConstantBufferBindRange;
using dxmt::ConstantBufferRange;
using dxmt::ConstantBufferRebindChanges;

void
TestSameBufferReboundPlain() {
  // *SetConstantBuffers1(first 256, num 64), then the same 64 KB buffer through the plain call
  const uint32_t first[] = {256}, num[] = {64};
  ConstantBufferRange bound = ConstantBufferBindRange(first, num, 0, 65536);
  CHECK_EQ(bound.FirstConstant, 256);
  CHECK_EQ(bound.NumConstants, 64);
  ConstantBufferRange plain = ConstantBufferBindRange(nullptr, nullptr, 0, 65536);
  CHECK_EQ(plain.FirstConstant, 0);
  CHECK_EQ(plain.NumConstants, 4096); // 65536 / 16, the most one binding addresses
  auto changes = ConstantBufferRebindChanges(bound, plain);
  CHECK(changes.offset_changed); // the offset goes back to 0 (re-emitted)
  CHECK(changes.size_changed);
  // bound plain twice: nothing changes
  auto again = ConstantBufferRebindChanges(plain, ConstantBufferBindRange(nullptr, nullptr, 0, 65536));
  CHECK(!again.offset_changed);
  CHECK(!again.size_changed);
}

void
TestNullNumConstantsWithFirstConstant() {
  // pNumConstants NULL, pFirstConstant given: the offset as given, the buffer's whole size
  const uint32_t first[] = {0, 32};
  ConstantBufferRange range = ConstantBufferBindRange(first, nullptr, 1, 1024);
  CHECK_EQ(range.FirstConstant, 32);
  CHECK_EQ(range.NumConstants, 64);
  // and the other way round
  const uint32_t num[] = {16};
  range = ConstantBufferBindRange(nullptr, num, 0, 1024);
  CHECK_EQ(range.FirstConstant, 0);
  CHECK_EQ(range.NumConstants, 16);
}

void
TestDifferentBuffer() {
  // a new buffer in the slot: its own range, from its start, whatever the slot held
  const uint32_t first[] = {512}, num[] = {128};
  ConstantBufferRange old_range = ConstantBufferBindRange(first, num, 0, 65536);
  ConstantBufferRange fresh = ConstantBufferBindRange(nullptr, nullptr, 0, 256);
  CHECK_EQ(fresh.FirstConstant, 0);
  CHECK_EQ(fresh.NumConstants, 16);
  CHECK(ConstantBufferRebindChanges(old_range, fresh).offset_changed);
  // a buffer smaller than one constant addresses none
  CHECK_EQ(ConstantBufferBindRange(nullptr, nullptr, 0, 8).NumConstants, 0);
}

void
TestNullBuffer() {
  // a NULL buffer unbinds the slot; the range rule isn't asked (the call site checks the
  // pointer first). A later plain bind of a buffer starts at 0 again.
  ConstantBufferRange after = ConstantBufferBindRange(nullptr, nullptr, 0, 4096);
  CHECK_EQ(after.FirstConstant, 0);
  CHECK_EQ(after.NumConstants, 256);
}

void
TestOffsetsInUseLine() {
  std::atomic<bool> seen{false};
  const uint32_t zeros[] = {0, 0};
  const uint32_t some[] = {0, 16};
  CHECK(!dxmt::FirstConstantBufferOffsetInUse(nullptr, 2, seen)); // plain call
  CHECK(!dxmt::FirstConstantBufferOffsetInUse(zeros, 2, seen));   // offsets all 0
  CHECK(dxmt::FirstConstantBufferOffsetInUse(some, 2, seen));     // the first non-zero one
  CHECK(!dxmt::FirstConstantBufferOffsetInUse(some, 2, seen));    // once per process
}

} // namespace

int
main() {
  TestSameBufferReboundPlain();
  TestNullNumConstantsWithFirstConstant();
  TestDifferentBuffer();
  TestNullBuffer();
  TestOffsetsInUseLine();
  return TestSummary("test_constant_buffer_bind");
}
