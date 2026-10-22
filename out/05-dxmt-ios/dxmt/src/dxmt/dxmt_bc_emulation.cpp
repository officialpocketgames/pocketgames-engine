// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 Part C, Windows half. See dxmt_bc_emulation.hpp.

#include "dxmt_bc_emulation.hpp"
#include "dxmt_bc_decode.hpp"
#include <atomic>

namespace dxmt {

bool
BCEmulationActive(WMT::Device device) {
#ifdef DXMT_IOS
  /* 0 = unknown, 1 = native BC, 2 = emulated. iOS has a single device. */
  static std::atomic<int> state = 0;
  int s = state.load(std::memory_order_relaxed);
  if (s == 0) {
    s = device.supportsBCTextureCompression() ? 1 : 2;
    state.store(s, std::memory_order_relaxed);
  }
  return s == 2;
#else
  (void)device;
  return false;
#endif
}

static bool
DecodeFormatFor(WMTPixelFormat format, bc::Format &out) {
  switch (format) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
    out = bc::Format::BC1;
    return true;
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
    out = bc::Format::BC2;
    return true;
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
    out = bc::Format::BC3;
    return true;
  case WMTPixelFormatBC4_RUnorm:
    out = bc::Format::BC4_UNORM;
    return true;
  case WMTPixelFormatBC4_RSnorm:
    out = bc::Format::BC4_SNORM;
    return true;
  case WMTPixelFormatBC5_RGUnorm:
    out = bc::Format::BC5_UNORM;
    return true;
  case WMTPixelFormatBC5_RGSnorm:
    out = bc::Format::BC5_SNORM;
    return true;
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    out = bc::Format::BC7;
    return true;
  default:
    return false;
  }
}

static bool
IsBC6H(WMTPixelFormat format) {
  return format == WMTPixelFormatBC6H_RGBFloat || format == WMTPixelFormatBC6H_RGBUfloat;
}

WMTPixelFormat
BCPhysicalFormat(WMTPixelFormat format) {
  switch (format) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC7_RGBAUnorm:
    return WMTPixelFormatRGBA8Unorm;
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    return WMTPixelFormatRGBA8Unorm_sRGB;
  case WMTPixelFormatBC4_RUnorm:
    return WMTPixelFormatR8Unorm;
  case WMTPixelFormatBC4_RSnorm:
    return WMTPixelFormatR8Snorm;
  case WMTPixelFormatBC5_RGUnorm:
    return WMTPixelFormatRG8Unorm;
  case WMTPixelFormatBC5_RGSnorm:
    return WMTPixelFormatRG8Snorm;
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC6H_RGBUfloat:
    return WMTPixelFormatRGBA16Float;
  default:
    return format;
  }
}

bool
IsEmulatedBCFormat(WMT::Device device, WMTPixelFormat format) {
  return BCPhysicalFormat(format) != format && BCEmulationActive(device);
}

uint32_t
BCPhysicalBytesPerTexel(WMTPixelFormat format) {
  if (IsBC6H(format))
    return 8;
  bc::Format f;
  return DecodeFormatFor(format, f) ? bc::DecodedBytesPerTexel(f) : 0;
}

uint32_t
BCBlockBytes(WMTPixelFormat format) {
  if (IsBC6H(format))
    return 16;
  bc::Format f;
  return DecodeFormatFor(format, f) ? bc::BlockBytes(f) : 0;
}

bool
BCDecodeToPhysical(
    WMTPixelFormat format, const void *src, size_t src_row_pitch, void *dst, size_t dst_row_pitch, uint32_t width,
    uint32_t height
) {
  if (IsBC6H(format)) {
    bc::FillBC6HGrey(dst, dst_row_pitch, width, height);
    return dst != nullptr;
  }
  bc::Format f;
  if (!DecodeFormatFor(format, f))
    return false;
  return bc::DecodeSubresource(f, src, src_row_pitch, dst, dst_row_pitch, width, height);
}

bool
BCDecodeVolumeToPhysical(
    WMTPixelFormat format, const void *src, size_t src_row_pitch, size_t src_depth_pitch, void *dst, uint32_t width,
    uint32_t height, uint32_t depth
) {
  if (IsBC6H(format)) {
    bc::FillBC6HGrey(dst, 0, width, height * depth); // tight slices are one tall image
    return dst != nullptr;
  }
  bc::Format f;
  if (!DecodeFormatFor(format, f))
    return false;
  return bc::DecodeVolume(f, src, src_row_pitch, src_depth_pitch, dst, 0, 0, width, height, depth);
}

} // namespace dxmt
