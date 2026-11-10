// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 21: the frame trace's plain part (dxmt/src/dxmt/dxmt_frame_trace.hpp):
// the switch value, which presents are traced, the line formats, the 600-line cap with its
// closing lines, and the 900-byte line limit.

#include "test_common.hpp"

#include "dxmt_frame_trace.hpp"

#include <string>
#include <vector>

namespace {

using namespace dxmt;

bool
StartsWith(const std::string &s, const std::string &prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

void
TestSwitchAndFrames() {
  CHECK(FrameTraceSwitchValue("1"));
  CHECK(!FrameTraceSwitchValue(""));
  CHECK(!FrameTraceSwitchValue("0"));
  CHECK(!FrameTraceSwitchValue("yes"));
  CHECK(IsTracedPresent(600));
  CHECK(IsTracedPresent(3600));
  CHECK(!IsTracedPresent(599));
  CHECK(!IsTracedPresent(601));
  CHECK(!IsTracedPresent(0));
  const uint8_t sha1[20] = {0x1a, 0x2b, 0x3c, 0x4d, 0x5e};
  CHECK_EQ(FrameTraceShaderHash(sha1), 0x1a2b3c4d);
}

void
TestFormats() {
  CHECK(TraceRect(0, 0, 1280, 720) == "0,0 1280x720");
  CHECK(TraceRect(0, 0, 768.5, 620) == "0,0 768.50x620");
  CHECK(TraceCovers(0, 0, 1280, 720, 1280, 720));
  CHECK(!TraceCovers(0, 0, 768, 620, 1280, 720));
  CHECK(!TraceCovers(8, 0, 1280, 720, 1280, 720));
  CHECK(TracePixelFormat(WMTPixelFormatRGBA16Float) == "RGBA16Float");
  CHECK(TracePixelFormat(WMTPixelFormatDepth32Float_Stencil8) == "Depth32Float_Stencil8");
  CHECK(TracePixelFormat(WMTPixelFormatBGRX8Unorm) == "BGRA8Unorm"); // swizzle flags dropped
  CHECK(TracePixelFormat(WMTPixelFormat(999)) == "format 999");
  CHECK(TraceAttachment("c0", WMTPixelFormatRGBA16Float, 1280, 720, 0, 0, WMTLoadActionClear) ==
        "c0 RGBA16Float 1280x720 mip 0 slice 0 clear");

  std::vector<std::string> viewports, scissors;
  TraceAddDistinct(viewports, TraceRect(0, 0, 1280, 720));
  TraceAddDistinct(viewports, TraceRect(0, 0, 1280, 720)); // distinct only
  TraceAddDistinct(scissors, TraceRect(0, 0, 768, 620));
  TraceAddDistinct(scissors, TraceRect(0, 0, 1011, 620));
  CHECK_EQ(viewports.size(), 1);
  CHECK(TraceRenderPass(3, {"c0 RGBA16Float 1280x720 mip 0 slice 0 load", "depth Depth32Float 1280x720 mip 0 slice 0 clear"},
                        12, viewports, scissors, true) ==
        "pass 3: c0 RGBA16Float 1280x720 mip 0 slice 0 load, depth Depth32Float 1280x720 mip 0 slice 0 clear; "
        "12 draws; viewports 0,0 1280x720; scissors 0,0 768x620 | 0,0 1011x620; PARTIAL");
  CHECK(TraceRenderPass(0, {}, 1, {}, {}, false) == "pass 0: no attachments; 1 draw");

  CHECK(TraceDispatch(0x1a2b3c4d, 8, 8, 1, false, 160, 90, 1, "u0 RGBA16Float 1280x720") ==
        "dispatch cs 1a2b3c4d tg 8x8x1 groups 160x90x1; u0 RGBA16Float 1280x720");
  CHECK(TraceDispatch(0x00000abc, 64, 1, 1, true, 0, 0, 0, "") == "dispatch cs 00000abc tg 64x1x1 indirect");
  CHECK(TraceDispatch(0, 32, 1, 1, false, 4, 1, 1, "") == "dispatch internal tg 32x1x1 groups 4x1x1");
  CHECK(TraceBox(0, 0, 0, 256, 256, 1) == "0,0,0 256x256x1");
}

void
TestFrameAndCap() {
  FrameTraceLog log;
  std::vector<std::string> out;
  log.line("not traced", out);
  CHECK(out.empty()); // outside a traced frame nothing is recorded

  log.setPresent(600, out);
  CHECK(log.active());
  CHECK_EQ(log.nextPass(), 0);
  CHECK_EQ(log.nextPass(), 1);
  log.countOffsetBind();
  log.countOffsetBind();
  for (unsigned i = 0; i < kFrameTraceMaxLines + 25; i++)
    log.line("pass " + std::to_string(i), out);
  CHECK_EQ(out.size(), kFrameTraceMaxLines);
  CHECK(out.front() == "dxmt-frame 600: pass 0");
  CHECK(out.back() == "dxmt-frame 600: pass 599");

  // the next frame closes it: the offset count, then how many lines weren't shown
  out.clear();
  log.setPresent(0, out);
  CHECK(!log.active());
  CHECK_EQ(out.size(), 2);
  CHECK(out[0] == "dxmt-frame 600: constant-buffer binds with a non-zero offset: 2");
  CHECK(out[1] == std::string("dxmt-frame 600: ") + kLogEllipsis + " 25 more not shown");

  // the second traced frame starts from zero; no "not shown" line when nothing was dropped
  out.clear();
  log.setPresent(3600, out);
  CHECK(out.empty());
  CHECK_EQ(log.nextPass(), 0);
  log.line("present", out);
  log.setPresent(0, out);
  CHECK_EQ(out.size(), 2);
  CHECK(out[0] == "dxmt-frame 3600: present");
  CHECK(out[1] == "dxmt-frame 3600: constant-buffer binds with a non-zero offset: 0");
  log.countOffsetBind(); // ignored outside a traced frame
}

void
TestLineLimit() {
  FrameTraceLog log;
  std::vector<std::string> out;
  log.setPresent(600, out);
  log.line(std::string(5000, 'x'), out);
  CHECK_EQ(out.size(), 1);
  CHECK(out[0].size() <= kFrameTraceMaxLineBytes);
  CHECK(StartsWith(out[0], "dxmt-frame 600: xxx"));
  // with the logger's own prefix the message stays within the 900-byte cap
  CHECK(FormatLogLine("info:  ", out[0]).size() <= kLogMessageMaxBytes);
}

} // namespace

int
main() {
  TestSwitchAndFrames();
  TestFormats();
  TestFrameAndCap();
  TestLineLimit();
  return TestSummary("test_frame_trace");
}
