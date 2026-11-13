// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 65 (DXMT_D11_STATE_FILTER, default on): when IASetInputLayout,
// IASetIndexBuffer, OMSetDepthStencilState or OMSetBlendState set what is already set, they
// change nothing. These are the comparisons, free of Direct3D types for the host test
// (tests/test_d3d11_cuts.cpp); the context calls them with its state.

#pragma once

#include <cstdint>
#include <cstring>

namespace dxmt {

/* OMSetBlendState: the same state, the same four factors (a null `factor` meaning 1, 1, 1, 1)
   and the same sample mask. Factors compare by their bits, so any change counts. */
inline bool
SameBlendCall(
    const void *current_state, const float current_factor[4], uint32_t current_mask, const void *state,
    const float *factor, uint32_t mask
) {
  static const float kOnes[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  return current_state == state && current_mask == mask &&
         std::memcmp(current_factor, factor ? factor : kOnes, sizeof(float[4])) == 0;
}

/* OMSetDepthStencilState: the same state (or none both times) and the same stencil reference. */
inline bool
SameDepthStencilCall(const void *current_state, uint32_t current_ref, const void *state, uint32_t ref) {
  return current_state == state && current_ref == ref;
}

/* IASetIndexBuffer: the buffer already set; the format and offset are stored either way. A call
   counts as repeated (clarification 63) when all three are the same. */
inline bool
SameIndexBuffer(const void *current_buffer, const void *buffer) {
  return current_buffer == buffer;
}

/* Whether the call does nothing more: the switch on and the value already set. */
constexpr bool
StateFilterSkips(bool filter_on, bool same) {
  return filter_on && same;
}

} // namespace dxmt
