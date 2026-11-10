// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 17: D3D11_FEATURE_FORMAT_SUPPORT2's UAV bits over the Apple7 format
// table (dxmt_format.cpp's FormatCapabilityInspector, with a fake device that reports only
// MTLGPUFamilyApple7), and the once-per-format rule and text of the read-write UAV log line.

#include "test_common.hpp"

// d3d11.h (mingw-directx-headers, upstream's copy) has an empty extern "C" struct; that is
// the header's, not the code under test.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#endif
#include "d3d11_format_support2.hpp"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "dxmt_uav_format_log.hpp"
#include "log/log.hpp"

#include <string>

extern "C" {
// the fake device: an Apple7 GPU (iPhone 12 class), nothing newer
bool
MTLDevice_supportsFamily(obj_handle_t, enum WMTGPUFamily gpu_family) {
  return gpu_family == WMTGPUFamilyApple7;
}
bool
MTLDevice_supportsBCTextureCompression(obj_handle_t) {
  return false;
}
} // extern "C"

// dxmt_format.cpp's warning when a GPU reports no family (not reached with this device)
void
dxmt::Logger::warn(const std::string &) {}

namespace {

constexpr UINT kLoad = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
constexpr UINT kStore = D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;

dxmt::FormatCapability
Apple7(WMTPixelFormat format) {
  static dxmt::FormatCapabilityInspector table = [] {
    dxmt::FormatCapabilityInspector t;
    t.Inspect(WMT::Device{});
    return t;
  }();
  auto found = table.textureCapabilities.find(format);
  return found == table.textureCapabilities.end() ? dxmt::FormatCapability::None : found->second;
}

UINT
Support2(DXGI_FORMAT dxgi, WMTPixelFormat metal) {
  return dxmt::D3D11FormatSupport2(dxgi, Apple7(metal));
}

void
TestTypedUAVBits() {
  // read or write, not read-write: stores only
  UINT rg11b10 = Support2(DXGI_FORMAT_R11G11B10_FLOAT, WMTPixelFormatRG11B10Float);
  CHECK((rg11b10 & kLoad) == 0);
  CHECK((rg11b10 & kStore) != 0);
  UINT rgb10a2 = Support2(DXGI_FORMAT_R10G10B10A2_UNORM, WMTPixelFormatRGB10A2Unorm);
  CHECK((rgb10a2 & kLoad) == 0);
  CHECK((rgb10a2 & kStore) != 0);

  // read-write: both
  for (auto [dxgi, metal] : {std::pair{DXGI_FORMAT_R16G16B16A16_FLOAT, WMTPixelFormatRGBA16Float},
                             std::pair{DXGI_FORMAT_R8G8B8A8_UNORM, WMTPixelFormatRGBA8Unorm},
                             std::pair{DXGI_FORMAT_R32_FLOAT, WMTPixelFormatR32Float}}) {
    UINT support = Support2(dxgi, metal);
    CHECK((support & kLoad) != 0);
    CHECK((support & kStore) != 0);
  }

  // read only (BGRA8Unorm has TextureBufferRead alone): no longer a typed load either
  UINT bgra8 = Support2(DXGI_FORMAT_B8G8R8A8_UNORM, WMTPixelFormatBGRA8Unorm);
  CHECK((bgra8 & (kLoad | kStore)) == 0);

  // structured and raw buffers: as before
  CHECK_EQ(dxmt::D3D11FormatSupport2(DXGI_FORMAT_UNKNOWN, dxmt::FormatCapability::None),
           D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
               D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
               D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE | D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
               D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX | kLoad | kStore |
               D3D11_FORMAT_SUPPORT2_SHAREABLE);

  // the other bits are unchanged: R32Uint keeps its atomics, RGBA16Float its tiled bit
  UINT r32u = Support2(DXGI_FORMAT_R32_UINT, WMTPixelFormatR32Uint);
  CHECK((r32u & D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD) != 0);
  CHECK((r32u & kLoad) != 0);
  CHECK((Support2(DXGI_FORMAT_R16G16B16A16_FLOAT, WMTPixelFormatRGBA16Float) & D3D11_FORMAT_SUPPORT2_TILED) != 0);
}

void
TestReadWriteUAVLine() {
  dxmt::ReadWriteUAVFormatLog log;
  // a format Metal reads and writes: never a line
  CHECK(!log.firstUnsupported(WMTPixelFormatRGBA16Float, Apple7(WMTPixelFormatRGBA16Float)));
  CHECK(!log.firstUnsupported(WMTPixelFormatRGBA16Float, Apple7(WMTPixelFormatRGBA16Float)));
  // one that it doesn't: the first time only, per format
  CHECK(log.firstUnsupported(WMTPixelFormatRG11B10Float, Apple7(WMTPixelFormatRG11B10Float)));
  CHECK(!log.firstUnsupported(WMTPixelFormatRG11B10Float, Apple7(WMTPixelFormatRG11B10Float)));
  CHECK(log.firstUnsupported(WMTPixelFormatRGB10A2Unorm, Apple7(WMTPixelFormatRGB10A2Unorm)));
  // a view's AlphaIsOne flag is not part of the key
  CHECK_EQ(dxmt::UAVBaseFormat(WMTPixelFormatBGRX8Unorm), WMTPixelFormatBGRA8Unorm);

  CHECK(dxmt::ReadWriteUAVLogLine(WMTPixelFormatRG11B10Float) ==
        "dxmt: a shader reads and writes a RG11B10Float UAV, which Metal can't do; its results are undefined");
  CHECK(dxmt::ReadWriteUAVLogLine(WMTPixelFormat(9999)) ==
        "dxmt: a shader reads and writes a pixel format 9999 UAV, which Metal can't do; its results are undefined");
}

} // namespace

int
main() {
  TestTypedUAVBits();
  TestReadWriteUAVLine();
  return TestSummary("test_format_support2");
}
