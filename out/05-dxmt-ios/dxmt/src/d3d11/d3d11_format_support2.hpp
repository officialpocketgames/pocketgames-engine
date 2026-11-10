// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 17: D3D11_FEATURE_FORMAT_SUPPORT2's answer for one format, as a pure
// function of the format's Metal capabilities, so the host tests check it over the format table.
// The mapping was in MTLD3D11Device::CheckFeatureSupport (upstream DXMT a3a6e8f, MIT); the
// change is that UAV_TYPED_LOAD now needs TextureBufferReadWrite: games read the bit as "a
// shader may load from and store to the same typed UAV", which Metal allows only for formats it
// can read and write through one texture (access::read_write). UAV_TYPED_STORE is unchanged.

#pragma once

#include "dxmt_format.hpp"
#include "d3d11.h"

namespace dxmt {

inline UINT
D3D11FormatSupport2(DXGI_FORMAT Format, FormatCapability Capability) {
  constexpr UINT kAtomics =
      D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
      D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
      D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;

  // structured and raw buffers: everything, as before
  if (Format == DXGI_FORMAT_UNKNOWN)
    return kAtomics | D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE |
           D3D11_FORMAT_SUPPORT2_SHAREABLE;

  auto has = [Capability](FormatCapability Bit) { return (int(Capability) & int(Bit)) != 0; };
  UINT Support = 0;

  // spec 05 clarification 17: loads only where Metal can read and write the same texture
  if (has(FormatCapability::TextureBufferReadWrite))
    Support |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
  if (has(FormatCapability::TextureBufferWrite))
    Support |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;

  if (has(FormatCapability::Atomic))
    Support |= kAtomics | D3D11_FORMAT_SUPPORT2_SHAREABLE;

#ifndef DXMT_NO_PRIVATE_API
  if (has(FormatCapability::Blend))
    Support |= D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP; /* UNCHECKED (upstream's note) */
#endif

  if (has(FormatCapability::Sparse))
    Support |= D3D11_FORMAT_SUPPORT2_TILED;

  return Support;
}

} // namespace dxmt
