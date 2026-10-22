// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Which swap-chain descriptions DXMT presents the way the game asked for.
//
// DXMT has one back buffer per swap chain whatever the description says: each
// Present copies it into the Metal layer's next drawable, and the layer's own
// drawable pool is the real flip queue. The back buffer keeps its contents
// after Present (it holds the frame just shown). That matches:
//   - DISCARD and FLIP_DISCARD with any buffer count (contents after Present
//     are undefined, so keeping them is allowed);
//   - FLIP_SEQUENTIAL with 2 or more buffers: Present shows the whole buffer,
//     and the buffer the game draws into next holds a frame it already
//     presented. With a real flip queue that would be an older frame; here it
//     is the newest one, which is the same for a game that redraws the frame
//     and a superset of what an app that only redraws changed regions expects.
//     Partial presents (dirty rectangles, scroll) are shown in full, which is
//     correct because the buffer holds the complete frame;
//   - SEQUENTIAL (blit model) with one buffer.
// Anything else (blit-model SEQUENTIAL with several buffers, whose buffers
// rotate their contents, or an unknown effect with several buffers) is drawn
// the same way but logged once as not exactly what was asked.
//
// Pure, without Windows types, so the host tests build it (DXGI_SWAP_EFFECT
// values from Microsoft's dxgi.h documentation).

#pragma once

#include <cstdint>

namespace dxmt {

constexpr uint32_t kDxgiSwapEffectDiscard = 0;
constexpr uint32_t kDxgiSwapEffectSequential = 1;
constexpr uint32_t kDxgiSwapEffectFlipSequential = 3;
constexpr uint32_t kDxgiSwapEffectFlipDiscard = 4;
constexpr uint32_t kDxgiMaxSwapChainBuffers = 16;

inline bool
SwapEffectHandledAsAsked(uint32_t swap_effect, uint32_t buffer_count) {
  switch (swap_effect) {
  case kDxgiSwapEffectDiscard:
  case kDxgiSwapEffectFlipDiscard:
    return true;
  case kDxgiSwapEffectFlipSequential:
    // The flip model needs 2 to 16 buffers (DXGI rejects other counts).
    return buffer_count >= 2 && buffer_count <= kDxgiMaxSwapChainBuffers;
  default:
    return buffer_count == 1;
  }
}

} // namespace dxmt
