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
  // spec 05 clarification 24: 600, then every 1200th, at most 8
  unsigned traced = 0;
  uint64_t last = 0;
  for (uint64_t present = 0; present <= 20000; present++) {
    if (IsTracedPresent(present)) {
      traced++;
      last = present;
    }
  }
  CHECK_EQ(traced, 8);
  CHECK_EQ(last, 9000);
  CHECK(IsTracedPresent(600));
  CHECK(IsTracedPresent(1800));
  CHECK(IsTracedPresent(3000));
  CHECK(!IsTracedPresent(3600)); // clarification 21's second frame no longer
  CHECK(!IsTracedPresent(599));
  CHECK(!IsTracedPresent(601));
  CHECK(!IsTracedPresent(1200));
  CHECK(!IsTracedPresent(10200)); // a 9th
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

  CHECK(TraceDispatch(4, 0x1a2b3c4d, 8, 8, 1, false, 160, 90, 1, "u0 RGBA16Float 1280x720") ==
        "dispatch #4 cs 1a2b3c4d tg 8x8x1 groups 160x90x1; u0 RGBA16Float 1280x720");
  CHECK(TraceDispatch(5, 0x00000abc, 64, 1, 1, true, 0, 0, 0, "") == "dispatch #5 cs 00000abc tg 64x1x1 indirect");
  CHECK(TraceDispatch(0, 0, 32, 1, 1, false, 4, 1, 1, "") == "dispatch internal tg 32x1x1 groups 4x1x1");

  // spec 05 clarification 27: the UAV read-back line, and both kinds through TraceReadbackLine
  uint32_t uav_values[kFrameTraceUAVReadbackValues];
  for (unsigned i = 0; i < kFrameTraceUAVReadbackValues; i++)
    uav_values[i] = i == 0 ? 0xdeadbeefu : i;
  std::string uav_line = TraceUAVReadback(1800, 4, 0, uav_values, kFrameTraceUAVReadbackValues);
  CHECK(StartsWith(uav_line, "dxmt-frame 1800: uav #4 u0 deadbeef 00000001 00000002 "));
  CHECK(uav_line.size() == std::string("dxmt-frame 1800: uav #4 u0").size() + 9 * kFrameTraceUAVReadbackValues);
  CHECK(uav_line.size() <= kFrameTraceMaxLineBytes);
  FrameTraceReadback uav{1800, 4, 3, false, uav_values};
  uav.uav = true;
  uav.slot = 2;
  CHECK(TraceReadbackLine(uav) == "dxmt-frame 1800: uav #4 u2 deadbeef 00000001 00000002");
  const uint32_t args[] = {40, 23, 1};
  CHECK(TraceReadbackLine({1800, 7, 3, false, args}) == "dxmt-frame 1800: indirect #7 args 40 23 1");
  CHECK(TraceBox(0, 0, 0, 256, 256, 1) == "0,0,0 256x256x1");

  // spec 05 clarification 24
  CHECK(TraceIndirect(3, "dispatch", 4096, 48, 1) ==
        "indirect #3 dispatch, argument buffer 4096 bytes, offset 48, written earlier in this command buffer: yes");
  CHECK(TraceIndirect(4, "dispatch", 12, 0, 0) ==
        "indirect #4 dispatch, argument buffer 12 bytes, offset 0, written earlier in this command buffer: no");
  CHECK(TraceIndirect(5, "draw indexed", 640, 20, -1) == "indirect #5 draw indexed, argument buffer 640 bytes, offset 20");
  const uint32_t dispatch_args[] = {40, 23, 1};
  CHECK(TraceIndirectArgs(1800, 3, dispatch_args, 3, false) == "dxmt-frame 1800: indirect #3 args 40 23 1");
  const uint32_t draw_args[] = {36, 1, 0, 0};
  CHECK(TraceIndirectArgs(600, 1, draw_args, 4, false) == "dxmt-frame 600: indirect #1 args 36 1 0 0");
  const uint32_t indexed_args[] = {6, 128, 0, (uint32_t)-4, 2}; // BaseVertexLocation is signed
  CHECK(TraceIndirectArgs(600, 2, indexed_args, 5, true) == "dxmt-frame 600: indirect #2 args 6 128 0 -4 2");
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
  log.setPresent(1800, out);
  CHECK(out.empty());
  CHECK_EQ(log.nextPass(), 0);
  CHECK_EQ(log.nextIndirect(), 1); // indirect numbers start at 1 in each traced frame
  CHECK_EQ(log.nextIndirect(), 2);
  CHECK_EQ(log.nextDispatch(), 1); // so do dispatch numbers (clarification 27)
  CHECK_EQ(log.nextDispatch(), 2);
  log.line("present", out);
  log.setPresent(0, out);
  CHECK_EQ(out.size(), 2);
  CHECK(out[0] == "dxmt-frame 1800: present");
  CHECK(out[1] == "dxmt-frame 1800: constant-buffer binds with a non-zero offset: 0");
  log.setPresent(3000, out);
  CHECK_EQ(log.nextIndirect(), 1);
  CHECK_EQ(log.nextDispatch(), 1);
  log.setPresent(0, out);
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
