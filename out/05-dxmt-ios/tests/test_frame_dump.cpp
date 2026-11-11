// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 28: the frame dump's plain part (dxmt/src/dxmt/dxmt_frame_dump.hpp):
// when it is on (both switches, present 1800 only), which formats are dumped and their texel
// sizes, file and directory names, and the 16 MB / 256 MB limits with their summary line.
// No image is made or read here: the dump only runs on a device, in test builds.

#include "test_common.hpp"

#include "dxmt_frame_dump.hpp"

#include <string>

namespace {

using namespace dxmt;

void
TestSwitchOff() {
  // off unless both switches are on, and only at present 1800
  CHECK(!FrameDumpActive(false, false, 1800));
  CHECK(!FrameDumpActive(false, true, 1800)); // the dump switch unset: never
  CHECK(!FrameDumpActive(true, false, 1800)); // the trace off: never
  CHECK(FrameDumpActive(true, true, 1800));
  CHECK(!FrameDumpActive(true, true, 600));
  CHECK(!FrameDumpActive(true, true, 3000));
  CHECK(!FrameDumpActive(true, true, 1799));
  CHECK(!FrameDumpActive(true, true, 0));
  CHECK(IsTracedPresent(kFrameDumpPresent)); // the dumped frame is a traced one
  CHECK(FrameTraceSwitchValue("1"));
  CHECK(!FrameTraceSwitchValue("")); // PG_DXMT_FRAME_DUMP unset reads ""
}

void
TestFormats() {
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA16Float), 8);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA8Unorm), 4);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA8Unorm_sRGB), 4);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRG11B10Float), 4);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatR16Float), 2);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatR8Snorm), 1);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA32Float), 16);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatBGRX8Unorm), 4); // swizzle flag dropped
  // not dumped: integer, depth, stencil, compressed
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatR32Uint), 0);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA8Uint), 0);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatDepth32Float), 0);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatDepth32Float_Stencil8), 0);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatStencil8), 0);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatBC1_RGBA), 0);
}

void
TestNames() {
  CHECK(FrameDumpDirectory(1800) == "C:\\users\\Public\\pg-dumps\\frame-1800");
  CHECK(FrameDumpFileName(7, "pass14", "c0", WMTPixelFormatRGBA16Float, 1280, 720) ==
        "0007-pass14-c0-RGBA16Float-1280x720.raw");
  CHECK(FrameDumpFileName(12, FrameDumpDispatchTag(0x7041a1e5), "u1", WMTPixelFormatR16Float, 640, 360) ==
        "0012-cs7041a1e5-u1-R16Float-640x360.raw");
  CHECK(FrameDumpDispatchTag(0xab) == "cs000000ab");
  // the names carry nothing but numbers, pass or shader hash, slot and format
  std::string name = FrameDumpFileName(1, "pass0", "c3", WMTPixelFormatRGB10A2Unorm, 1, 1);
  CHECK(name == "0001-pass0-c3-RGB10A2Unorm-1x1.raw");
  CHECK(name.find('\\') == std::string::npos && name.find('/') == std::string::npos);
}

void
TestLimits() {
  FrameDumpBudget budget;
  // one 1280x720 RGBA16Float target is 7.03 MB: admitted
  CHECK(budget.admit(1280ull * 720 * 8));
  // exactly 16 MB is admitted; one byte more is skipped as too large
  CHECK(budget.admit(16ull << 20));
  CHECK(!budget.admit((16ull << 20) + 1));
  CHECK_EQ(budget.tooLarge(), 1);
  // fill to the 256 MB total: then everything else is left out
  FrameDumpBudget total;
  for (int i = 0; i < 16; i++)
    CHECK(total.admit(16ull << 20));
  CHECK_EQ(total.total(), 256ull << 20);
  CHECK(!total.admit(4));
  CHECK(!total.admit(16ull << 20));
  CHECK(!total.admit(32ull << 20)); // over 16 MB counts as too large, not over the total
  CHECK_EQ(total.files(), 16);
  CHECK_EQ(total.overTotal(), 2);
  CHECK_EQ(total.tooLarge(), 1);
  CHECK(total.summary() == "dump: 16 files, 256.0 MB; left out: 1 over 16 MB, 2 over the 256 MB total");
  CHECK(FrameDumpBudget().summary() == "dump: 0 files, 0.0 MB; left out: 0 over 16 MB, 0 over the 256 MB total");
}

void
TestDepthPlanes() {
  // spec 05 clarification 31
  FrameDumpPlane planes[2];
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatDepth32Float, planes), 1);
  CHECK(std::string(planes[0].slot) == "depth");
  CHECK_EQ(planes[0].bytes_per_texel, 4);
  CHECK_EQ(planes[0].option, WMTBlitOptionNone);
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatDepth32Float_Stencil8, planes), 2);
  CHECK(std::string(planes[0].format_label) == "Depth32Float");
  CHECK_EQ(planes[0].bytes_per_texel, 4);
  CHECK_EQ(planes[0].option, WMTBlitOptionDepthFromDepthStencil);
  CHECK(std::string(planes[1].slot) == "stencil");
  CHECK(std::string(planes[1].format_label) == "Stencil8");
  CHECK_EQ(planes[1].bytes_per_texel, 1);
  CHECK_EQ(planes[1].option, WMTBlitOptionStencilFromDepthStencil);
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatDepth16Unorm, planes), 0);
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatRGBA8Unorm, planes), 0);

  CHECK(FrameDumpFileName(9, "pass3", "depth", "Depth32Float", 2048, 2048) ==
        "0009-pass3-depth-Depth32Float-2048x2048.raw");
  CHECK(FrameDumpFileName(10, "pass3", "stencil", "Stencil8", 1280, 720) == "0010-pass3-stencil-Stencil8-1280x720.raw");

  // a 2048x2048 Depth32Float shadow map is exactly 16 MB: included; 2049 wide is not
  FrameDumpBudget budget;
  CHECK_EQ(2048ull * 2048 * 4, 16ull << 20);
  CHECK(budget.admit(2048ull * 2048 * 4));
  CHECK(!budget.admit(2049ull * 2048 * 4));
  // a 1280x720 depth-stencil: its depth (3.5 MB) and stencil (0.9 MB) planes
  CHECK(budget.admit(1280ull * 720 * 4));
  CHECK(budget.admit(1280ull * 720 * 1));
  CHECK_EQ(budget.files(), 3);
}

} // namespace

int
main() {
  TestSwitchOff();
  TestFormats();
  TestNames();
  TestLimits();
  TestDepthPlanes();
  return TestSummary("test_frame_dump");
}
