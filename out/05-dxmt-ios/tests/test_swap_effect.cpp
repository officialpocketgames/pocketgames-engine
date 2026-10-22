// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Host test for the swap-chain descriptions DXMT presents as asked
// (dxmt/src/d3d11/d3d11_swap_effect.hpp): FLIP_SEQUENTIAL with 2 buffers, the
// usual Unity / D3D11 setup, is accepted without the "unsupported swap effect"
// warning, like FLIP_DISCARD.
//
//   c++ -std=c++20 -I../dxmt/src/d3d11 test_swap_effect.cpp -o test_swap_effect

#include "test_common.hpp"

#include "d3d11_swap_effect.hpp"

#include <initializer_list>

using namespace dxmt;

int
main() {
  // The DXGI_SWAP_EFFECT values.
  CHECK_EQ(kDxgiSwapEffectDiscard, 0);
  CHECK_EQ(kDxgiSwapEffectSequential, 1);
  CHECK_EQ(kDxgiSwapEffectFlipSequential, 3);
  CHECK_EQ(kDxgiSwapEffectFlipDiscard, 4);

  // FLIP_SEQUENTIAL: 2 to 16 buffers.
  CHECK(SwapEffectHandledAsAsked(kDxgiSwapEffectFlipSequential, 2));
  CHECK(SwapEffectHandledAsAsked(kDxgiSwapEffectFlipSequential, 3));
  CHECK(SwapEffectHandledAsAsked(kDxgiSwapEffectFlipSequential, 16));
  CHECK(!SwapEffectHandledAsAsked(kDxgiSwapEffectFlipSequential, 1));
  CHECK(!SwapEffectHandledAsAsked(kDxgiSwapEffectFlipSequential, 0));
  CHECK(!SwapEffectHandledAsAsked(kDxgiSwapEffectFlipSequential, 17));

  // DISCARD and FLIP_DISCARD: any count (unchanged).
  for (uint32_t n : {0u, 1u, 2u, 3u, 16u}) {
    CHECK(SwapEffectHandledAsAsked(kDxgiSwapEffectDiscard, n));
    CHECK(SwapEffectHandledAsAsked(kDxgiSwapEffectFlipDiscard, n));
  }

  // Blit-model SEQUENTIAL: one buffer only (unchanged).
  CHECK(SwapEffectHandledAsAsked(kDxgiSwapEffectSequential, 1));
  CHECK(!SwapEffectHandledAsAsked(kDxgiSwapEffectSequential, 2));

  // An unknown effect: one buffer only (unchanged).
  CHECK(SwapEffectHandledAsAsked(2, 1));
  CHECK(!SwapEffectHandledAsAsked(2, 2));
  CHECK(SwapEffectHandledAsAsked(99, 1));
  CHECK(!SwapEffectHandledAsAsked(99, 3));

  std::printf("test_swap_effect: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
