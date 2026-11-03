// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 10: host test of the integer render-target format table that steamac's
// DXMT patch 0017 needs (dxmt/src/airconv/dxbc_integer_rt.hpp). d3d11 passes airconv the
// MTLPixelFormat of every render target IsIntegerFormat (dxmt_format.cpp) calls integer, 0
// otherwise; airconv declares that pixel shader output uint4 for a Uint format, int4 for a Sint
// format. Checked against winemetal.h's WMTPixelFormat values (Metal's MTLPixelFormat numbers):
// each Uint and Sint color format, the color formats that aren't integer, and a sweep of every
// value up to 600 so nothing else is taken for an integer target.
//
//   c++ -std=c++20 -DDXMT_NATIVE=1 -I../dxmt/src/airconv -I../dxmt/src/winemetal test_integer_rt_formats.cpp

#include "test_common.hpp"

#include "dxbc_integer_rt.hpp"
#include "winemetal.h"

#include <iterator>

namespace {

using dxmt::dxbc::ClassifyIntegerRenderTarget;
using Kind = dxmt::dxbc::IntegerRenderTargetKind;

/* IsIntegerFormat's color formats (its list also has Stencil8, which is never a color
   target), split by component type */
constexpr WMTPixelFormat kUint[] = {
    WMTPixelFormatR8Uint,     WMTPixelFormatR16Uint,    WMTPixelFormatRG8Uint,      WMTPixelFormatR32Uint,
    WMTPixelFormatRG16Uint,   WMTPixelFormatRGBA8Uint,  WMTPixelFormatRGB10A2Uint,  WMTPixelFormatRG32Uint,
    WMTPixelFormatRGBA16Uint, WMTPixelFormatRGBA32Uint,
};
constexpr WMTPixelFormat kSint[] = {
    WMTPixelFormatR8Sint,    WMTPixelFormatR16Sint,    WMTPixelFormatRG8Sint,    WMTPixelFormatR32Sint,
    WMTPixelFormatRG16Sint,  WMTPixelFormatRGBA8Sint,  WMTPixelFormatRG32Sint,   WMTPixelFormatRGBA16Sint,
    WMTPixelFormatRGBA32Sint,
};
constexpr WMTPixelFormat kNotInteger[] = {
    WMTPixelFormatInvalid,        WMTPixelFormatR8Unorm,       WMTPixelFormatR8Snorm,
    WMTPixelFormatR16Float,       WMTPixelFormatRG8Unorm,      WMTPixelFormatR32Float,
    WMTPixelFormatRG16Float,      WMTPixelFormatRGBA8Unorm,    WMTPixelFormatRGBA8Unorm_sRGB,
    WMTPixelFormatRGBA8Snorm,     WMTPixelFormatBGRA8Unorm,    WMTPixelFormatBGRA8Unorm_sRGB,
    WMTPixelFormatRGB10A2Unorm,   WMTPixelFormatRG11B10Float,  WMTPixelFormatRG32Float,
    WMTPixelFormatRGBA16Unorm,    WMTPixelFormatRGBA16Float,   WMTPixelFormatRGBA32Float,
    WMTPixelFormatDepth32Float,   WMTPixelFormatStencil8,      WMTPixelFormatDepth32Float_Stencil8,
};

bool
Contains(const WMTPixelFormat *list, unsigned count, uint32_t value) {
  for (unsigned i = 0; i < count; i++) {
    if (uint32_t(list[i]) == value)
      return true;
  }
  return false;
}

void
TestListedFormats() {
  for (auto format : kUint)
    CHECK(ClassifyIntegerRenderTarget(format) == Kind::Uint);
  for (auto format : kSint)
    CHECK(ClassifyIntegerRenderTarget(format) == Kind::Sint);
  for (auto format : kNotInteger)
    CHECK(ClassifyIntegerRenderTarget(format) == Kind::None);
  // a pipeline without an integer target passes zeros
  CHECK(ClassifyIntegerRenderTarget(0) == Kind::None);
}

void
TestSweep() {
  unsigned uints = 0, sints = 0, wrong = 0;
  for (uint32_t value = 0; value <= 600; value++) {
    auto kind = ClassifyIntegerRenderTarget(value);
    if (kind == Kind::Uint) {
      uints++;
      if (!Contains(kUint, std::size(kUint), value))
        wrong++;
    } else if (kind == Kind::Sint) {
      sints++;
      if (!Contains(kSint, std::size(kSint), value))
        wrong++;
    }
  }
  CHECK_EQ(uints, std::size(kUint));
  CHECK_EQ(sints, std::size(kSint));
  CHECK_EQ(wrong, 0);
}

static_assert(ClassifyIntegerRenderTarget(WMTPixelFormatRGBA8Uint) == Kind::Uint, "usable in constant expressions");

} // namespace

int
main() {
  TestListedFormats();
  TestSweep();
  return TestSummary("test_integer_rt_formats");
}
