// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 17: the log line for a UAV a shader both reads and writes (the
// converter declares it access::read_write) bound with a format Metal can't read and write
// through one texture. Once per format per process. Plain code so the host tests drive it.

#pragma once

#include "dxmt_format.hpp"
#include <mutex>
#include <set>
#include <string>

namespace dxmt {

/* The Metal name without the "MTLPixelFormat" prefix for the colour formats a UAV can have,
   or nullptr. */
inline const char *
UAVPixelFormatName(WMTPixelFormat Format) {
  switch (Format) {
  case WMTPixelFormatA8Unorm: return "A8Unorm";
  case WMTPixelFormatR8Unorm: return "R8Unorm";
  case WMTPixelFormatR8Snorm: return "R8Snorm";
  case WMTPixelFormatR8Uint: return "R8Uint";
  case WMTPixelFormatR8Sint: return "R8Sint";
  case WMTPixelFormatR16Unorm: return "R16Unorm";
  case WMTPixelFormatR16Snorm: return "R16Snorm";
  case WMTPixelFormatR16Uint: return "R16Uint";
  case WMTPixelFormatR16Sint: return "R16Sint";
  case WMTPixelFormatR16Float: return "R16Float";
  case WMTPixelFormatRG8Unorm: return "RG8Unorm";
  case WMTPixelFormatRG8Snorm: return "RG8Snorm";
  case WMTPixelFormatRG8Uint: return "RG8Uint";
  case WMTPixelFormatRG8Sint: return "RG8Sint";
  case WMTPixelFormatR32Uint: return "R32Uint";
  case WMTPixelFormatR32Sint: return "R32Sint";
  case WMTPixelFormatR32Float: return "R32Float";
  case WMTPixelFormatRG16Unorm: return "RG16Unorm";
  case WMTPixelFormatRG16Snorm: return "RG16Snorm";
  case WMTPixelFormatRG16Uint: return "RG16Uint";
  case WMTPixelFormatRG16Sint: return "RG16Sint";
  case WMTPixelFormatRG16Float: return "RG16Float";
  case WMTPixelFormatRGBA8Unorm: return "RGBA8Unorm";
  case WMTPixelFormatRGBA8Unorm_sRGB: return "RGBA8Unorm_sRGB";
  case WMTPixelFormatRGBA8Snorm: return "RGBA8Snorm";
  case WMTPixelFormatRGBA8Uint: return "RGBA8Uint";
  case WMTPixelFormatRGBA8Sint: return "RGBA8Sint";
  case WMTPixelFormatBGRA8Unorm: return "BGRA8Unorm";
  case WMTPixelFormatBGRA8Unorm_sRGB: return "BGRA8Unorm_sRGB";
  case WMTPixelFormatRGB10A2Unorm: return "RGB10A2Unorm";
  case WMTPixelFormatBGR10A2Unorm: return "BGR10A2Unorm";
  case WMTPixelFormatRGB10A2Uint: return "RGB10A2Uint";
  case WMTPixelFormatRG11B10Float: return "RG11B10Float";
  case WMTPixelFormatRGB9E5Float: return "RGB9E5Float";
  case WMTPixelFormatRG32Uint: return "RG32Uint";
  case WMTPixelFormatRG32Sint: return "RG32Sint";
  case WMTPixelFormatRG32Float: return "RG32Float";
  case WMTPixelFormatRGBA16Unorm: return "RGBA16Unorm";
  case WMTPixelFormatRGBA16Snorm: return "RGBA16Snorm";
  case WMTPixelFormatRGBA16Uint: return "RGBA16Uint";
  case WMTPixelFormatRGBA16Sint: return "RGBA16Sint";
  case WMTPixelFormatRGBA16Float: return "RGBA16Float";
  case WMTPixelFormatRGBA32Uint: return "RGBA32Uint";
  case WMTPixelFormatRGBA32Sint: return "RGBA32Sint";
  case WMTPixelFormatRGBA32Float: return "RGBA32Float";
  default: return nullptr;
  }
}

/* A view's format as the capability table keys it (without the AlphaIsOne swizzle flag). */
inline WMTPixelFormat
UAVBaseFormat(WMTPixelFormat Format) {
  return static_cast<WMTPixelFormat>(Format & ~WMTPixelFormatAlphaIsOne);
}

inline std::string
ReadWriteUAVLogLine(WMTPixelFormat Format) {
  const char *name = UAVPixelFormatName(Format);
  std::string format = name ? std::string(name) : "pixel format " + std::to_string(uint32_t(Format));
  return "dxmt: a shader reads and writes a " + format + " UAV, which Metal can't do; its results are undefined";
}

class ReadWriteUAVFormatLog {
public:
  /* True the first time `Format` is seen without TextureBufferReadWrite: log the line then.
     A format that has it costs no lock. */
  bool
  firstUnsupported(WMTPixelFormat Format, FormatCapability Capability) {
    if (int(Capability) & int(FormatCapability::TextureBufferReadWrite))
      return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return logged_.insert(uint32_t(Format)).second;
  }

  /* the one per process */
  static ReadWriteUAVFormatLog &
  global() {
    static ReadWriteUAVFormatLog log;
    return log;
  }

private:
  std::mutex mutex_;
  std::set<uint32_t> logged_;
};

} // namespace dxmt
