// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 23: Metal.hpp's Device::newComputePipelineState(const Function &, ...)
// wrappers hand winemetal a WMTComputePipelineInfo with every field set, item 20's
// max_total_threads_per_threadgroup and padding_end included, even when the stack they build it
// on holds garbage. A fake MTLDevice_newComputePipelineState captures what they pass. Also the
// unix side's guard (winemetal/unix/compute_pipeline_limits.h) on the values that reach Metal.

#include "test_common.hpp"

#include "Metal.hpp"
#include "compute_pipeline_limits.h"

#include <cstring>

namespace {
WMTComputePipelineInfo g_captured;
unsigned g_calls = 0;
} // namespace

extern "C" {
obj_handle_t
MTLDevice_newComputePipelineState(obj_handle_t, const struct WMTComputePipelineInfo *info, obj_handle_t *err_out) {
  std::memcpy(&g_captured, info, sizeof(g_captured));
  g_calls++;
  if (err_out)
    *err_out = 0;
  return 0; // no pipeline: nothing for the Reference to release
}
void
NSObject_retain(obj_handle_t) {}
void
NSObject_release(obj_handle_t) {}
} // extern "C"

namespace {

/* Leaves 0xcd in the stack below the caller's frame, where the wrapper's info will sit. */
__attribute__((noinline)) void
DirtyStack() {
  volatile unsigned char garbage[16384];
  for (size_t i = 0; i < sizeof(garbage); i++)
    garbage[i] = 0xcd;
}

__attribute__((noinline)) void
CallWrapper(bool with_tgsize_flag) {
  WMT::Device device;
  device.handle = 0x100;
  WMT::Function function;
  function.handle = 0x200;
  WMT::Error error;
  if (with_tgsize_flag)
    device.newComputePipelineState(function, true, error);
  else
    device.newComputePipelineState(function, error);
}

void
CheckCaptured(bool tgsize_flag) {
  CHECK_EQ(g_captured.compute_function, 0x200);
  CHECK(g_captured.binary_archives_for_lookup.ptr == nullptr);
  CHECK_EQ(g_captured.binary_archive_for_serialization, 0);
  CHECK_EQ(g_captured.num_binary_archives_for_lookup, 0);
  CHECK(!g_captured.fail_on_binary_archive_miss);
  CHECK_EQ(g_captured.padding, 0);
  CHECK_EQ(g_captured.tgsize_is_multiple_of_sgwidth, tgsize_flag);
  CHECK_EQ(g_captured.immutable_buffers, 0);
  CHECK_EQ(g_captured.max_total_threads_per_threadgroup, 0); // the build-62 crash: garbage here
  CHECK_EQ(g_captured.padding_end, 0);
  // and the guard leaves Metal's default for it
  CHECK_EQ(dxmt_threads_per_threadgroup_use(g_captured.max_total_threads_per_threadgroup), DXMT_THREADS_DEFAULT);
}

void
TestWrappersOnDirtyStack() {
  for (bool flag : {false, true}) {
    std::memset(&g_captured, 0xcd, sizeof(g_captured));
    DirtyStack();
    CallWrapper(flag);
    CheckCaptured(flag);
  }
  CHECK_EQ(g_calls, 2);
}

void
TestInitializerSetsEveryField() {
  WMTComputePipelineInfo info;
  std::memset(&info, 0xcd, sizeof(info));
  WMT::InitializeComputePipelineInfo(info);
  WMTComputePipelineInfo zero;
  std::memset(&zero, 0, sizeof(zero));
  // every byte is set: all fields are 0 / NULL / false, and the struct has no implicit padding
  CHECK_EQ(std::memcmp(&info, &zero, sizeof(info)), 0);
}

void
TestGuard() {
  CHECK_EQ(dxmt_threads_per_threadgroup_use(0), DXMT_THREADS_DEFAULT);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(1), DXMT_THREADS_APPLY);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(64), DXMT_THREADS_APPLY);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(1024), DXMT_THREADS_APPLY);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(1025), DXMT_THREADS_IGNORED);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(65535), DXMT_THREADS_IGNORED);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(65536), DXMT_THREADS_IGNORED);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(0xffffffffu), DXMT_THREADS_IGNORED);
  CHECK_EQ(dxmt_threads_per_threadgroup_use(0xcdcdcdcdu), DXMT_THREADS_IGNORED);
}

} // namespace

int
main() {
  TestGuard();
  TestInitializerSetsEveryField();
  TestWrappersOnDirtyStack();
  return TestSummary("test_compute_pipeline_info");
}
