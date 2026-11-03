// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 10: the airconv side that steamac's DXMT patch 0017 needs (this tree's
// airconv had no way to type a pixel shader output after its render target). A pixel shader
// output bound to an integer render target is declared with the target's component type, so
// Metal accepts the pipeline; the register's bits are passed through unchanged, as D3D11
// hardware does when a shader writes a float-declared output to a Uint/Sint target (undefined
// in D3D11). The values are Metal's MTLPixelFormat numbers (Apple's MTLPixelFormat.h; the same
// numbers as WMTPixelFormat in winemetal.h), which d3d11 passes in
// SM50_SHADER_PSO_PIXEL_SHADER_DATA::pixel_formats, 0 for a target that isn't integer.
#pragma once

#include <cstdint>

namespace dxmt::dxbc {

enum class IntegerRenderTargetKind { None, Uint, Sint };

constexpr IntegerRenderTargetKind
ClassifyIntegerRenderTarget(uint32_t mtl_pixel_format) {
  switch (mtl_pixel_format) {
  case 13:  // R8Uint
  case 23:  // R16Uint
  case 33:  // RG8Uint
  case 53:  // R32Uint
  case 63:  // RG16Uint
  case 73:  // RGBA8Uint
  case 91:  // RGB10A2Uint
  case 103: // RG32Uint
  case 113: // RGBA16Uint
  case 123: // RGBA32Uint
    return IntegerRenderTargetKind::Uint;
  case 14:  // R8Sint
  case 24:  // R16Sint
  case 34:  // RG8Sint
  case 54:  // R32Sint
  case 64:  // RG16Sint
  case 74:  // RGBA8Sint
  case 104: // RG32Sint
  case 114: // RGBA16Sint
  case 124: // RGBA32Sint
    return IntegerRenderTargetKind::Sint;
  default:
    return IntegerRenderTargetKind::None;
  }
}

} // namespace dxmt::dxbc
