// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 25: an indirect dispatch is preceded, in its compute encoder, by a memory
// barrier over buffers and textures. Direct3D programs commonly build dispatch arguments in a
// compute shader through a typed-buffer UAV, which DXMT writes through a texture made from the
// buffer's storage, then dispatch indirectly from the buffer itself; Metal tracks the two
// objects separately, and nothing else orders them inside one serial compute encoder.
// A template over the encoder so the host tests check the record chain with a fake.

#pragma once

#include "winemetal.h"

namespace dxmt {

/* the scope of the barrier before an indirect dispatch */
constexpr uint8_t kIndirectDispatchBarrierScope = WMTBarrierScopeBuffers | WMTBarrierScopeTextures;

/* Encodes the barrier, then the indirect dispatch, and returns the dispatch record. `Encoder`
   has encodeComputeCommand<T>() (ArgumentEncodingContext). */
template <typename Encoder>
wmtcmd_compute_dispatch_indirect &
EncodeIndirectDispatch(Encoder &enc, obj_handle_t args_buffer, uint64_t args_offset) {
  auto &barrier = enc.template encodeComputeCommand<wmtcmd_compute_memory_barrier>();
  barrier.type = WMTComputeCommandMemoryBarrier;
  barrier.reserved[0] = barrier.reserved[1] = barrier.reserved[2] = 0;
  barrier.scope = kIndirectDispatchBarrierScope;
  for (auto &byte : barrier.padding)
    byte = 0;
  auto &cmd = enc.template encodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
  cmd.type = WMTComputeCommandDispatchIndirect;
  cmd.indirect_args_buffer = args_buffer;
  cmd.indirect_args_offset = args_offset;
  return cmd;
}

} // namespace dxmt
