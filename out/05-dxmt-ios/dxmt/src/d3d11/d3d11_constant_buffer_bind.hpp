// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 19: the range a *SetConstantBuffers / *SetConstantBuffers1 call binds.
// In Direct3D 11.1 a plain call (both pointers NULL) binds each buffer from its start, its whole
// size (at most 4096 constants), whatever a *SetConstantBuffers1 call set before; so does a
// NULL pointer of the two for its own half. Plain code so the host tests check it.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace dxmt {

/* the most constants one binding can address (D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT) */
constexpr uint32_t kMaxConstantsPerBinding = 4096;

struct ConstantBufferRange {
  uint32_t FirstConstant;
  uint32_t NumConstants;
};

/* What slot `index` of the call binds for a buffer of `byte_width` bytes. */
inline ConstantBufferRange
ConstantBufferBindRange(const uint32_t *pFirstConstant, const uint32_t *pNumConstants, uint32_t index,
                        uint32_t byte_width) {
  ConstantBufferRange range;
  range.FirstConstant = pFirstConstant ? pFirstConstant[index] : 0;
  range.NumConstants = pNumConstants ? pNumConstants[index] : std::min(byte_width >> 4, kMaxConstantsPerBinding);
  return range;
}

/* What changes when a slot that holds the same buffer gets `next`: the offset (re-emitted to the
   encoder), the size (only the slot is marked dirty). */
struct ConstantBufferRebind {
  bool offset_changed;
  bool size_changed;
};

inline ConstantBufferRebind
ConstantBufferRebindChanges(const ConstantBufferRange &bound, const ConstantBufferRange &next) {
  return {bound.FirstConstant != next.FirstConstant, bound.NumConstants != next.NumConstants};
}

/* The once-per-process "dxmt: constant buffer offsets in use" line: true for the first call (of
   any context) that binds a non-zero FirstConstant. */
inline bool
FirstConstantBufferOffsetInUse(const uint32_t *pFirstConstant, uint32_t count, std::atomic<bool> &seen) {
  if (!pFirstConstant || seen.load(std::memory_order_relaxed))
    return false;
  for (uint32_t i = 0; i < count; i++) {
    if (pFirstConstant[i] != 0)
      return !seen.exchange(true, std::memory_order_relaxed);
  }
  return false;
}

/* the process's flag for the line above */
inline std::atomic<bool> &
ConstantBufferOffsetsSeen() {
  static std::atomic<bool> seen{false};
  return seen;
}

} // namespace dxmt
