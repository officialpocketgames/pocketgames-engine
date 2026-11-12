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
  // clarification 39: no cap. Build 66's frame had about 2400 lines; all of them are kept
  for (unsigned i = 0; i < 2422; i++)
    log.line("pass " + std::to_string(i), out);
  CHECK_EQ(out.size(), 2422);
  CHECK(out.front() == "pass 0"); // no prefix: the file's folder names the frame
  CHECK(out.back() == "pass 2421");
  log.countRenderPass(12);
  log.countRenderPass(3);
  log.countDispatch();
  CHECK_EQ(log.renderPasses(), 2);
  CHECK_EQ(log.draws(), 15);
  CHECK_EQ(log.dispatchCount(), 1);

  // the next frame closes it: the offset count
  out.clear();
  log.setPresent(0, out);
  CHECK(!log.active());
  CHECK_EQ(out.size(), 1);
  CHECK(out[0] == "constant-buffer binds with a non-zero offset: 2");

  // the second traced frame starts from zero
  out.clear();
  log.setPresent(1800, out);
  CHECK(out.empty());
  CHECK_EQ(log.nextPass(), 0);
  CHECK_EQ(log.renderPasses(), 0);
  CHECK_EQ(log.nextIndirect(), 1); // indirect numbers start at 1 in each traced frame
  CHECK_EQ(log.nextIndirect(), 2);
  CHECK_EQ(log.nextDispatch(), 1); // so do dispatch numbers (clarification 27)
  CHECK_EQ(log.nextDispatch(), 2);
  log.line("present", out);
  log.setPresent(0, out);
  CHECK_EQ(out.size(), 2);
  CHECK(out[0] == "present");
  CHECK(out[1] == "constant-buffer binds with a non-zero offset: 0");
  log.setPresent(3000, out);
  CHECK_EQ(log.nextIndirect(), 1);
  CHECK_EQ(log.nextDispatch(), 1);
  log.setPresent(0, out);
  log.countOffsetBind(); // ignored outside a traced frame
}

void
TestTraceFile() {
  // clarification 39: a frame over the old 600-line cap reaches trace.txt whole, with each
  // read-back right after its line
  std::vector<std::string> lines;
  for (unsigned i = 0; i < 2400; i++)
    lines.push_back("pass " + std::to_string(i) + ": c0 RGBA16Float 1280x720 mip 0 slice 0 load; 3 draws");
  lines.insert(lines.begin() + 1000, "dispatch #7 cs 1a2b3c4d tg 8x8x1 groups 160x90x1; u0 R32Uint 4096 elements");
  lines.insert(lines.begin() + 1001, "dispatch #7 sampler cs s0 addr clamp/clamp/clamp ...");
  lines.insert(lines.begin() + 1500, "indirect #3 dispatch, argument buffer 12 bytes, offset 0, written earlier "
                                     "in this command buffer: yes");
  lines.push_back("constant-buffer binds with a non-zero offset: 0");
  uint32_t values[3] = {0xdeadbeef, 1, 2};
  FrameTraceReadback uav{1800, 7, 3, false, values};
  uav.uav = true;
  FrameTraceReadback args{1800, 3, 3, false, values};
  FrameTraceReadback stray{1800, 99, 1, false, values}; // no line of its own
  std::vector<FrameTraceReadbackText> readbacks = {TraceReadbackText(stray), TraceReadbackText(args),
                                                   TraceReadbackText(uav)};
  CHECK(readbacks[2].text == "uav #7 u0 deadbeef 00000001 00000002"); // without the log prefix
  CHECK(readbacks[1].text == "indirect #3 args 3735928559 1 2");

  auto file = FrameTraceFileLines(lines, readbacks);
  CHECK_EQ(file.size(), lines.size() + 3);
  CHECK(file[1000] == lines[1000]);
  CHECK(file[1001] == "uav #7 u0 deadbeef 00000001 00000002"); // right after its dispatch line, once
  CHECK(file[1002] == lines[1001]);
  CHECK(file[1501] == lines[1500]);
  CHECK(file[1502] == "indirect #3 args 3735928559 1 2");
  CHECK(file.back() == "indirect #99 args 3735928559"); // the stray one at the end
  CHECK(file[file.size() - 2] == "constant-buffer binds with a non-zero offset: 0");

  CHECK_EQ(TraceLineNumber("dispatch #12 cs 00000001", "dispatch #"), 12);
  CHECK_EQ(TraceLineNumber("dispatch internal tg 32x1x1", "dispatch #"), 0);
  CHECK_EQ(TraceLineNumber("indirect #3 args 1 1 1", "indirect #"), 3);
  CHECK_EQ(TraceLineNumber("indirect #", "indirect #"), 0);
}

void
TestDrawsAndLayers() {
  // clarification 44: the pass line's layers and attachment types
  CHECK(TraceAttachment("c0", WMTPixelFormatRGBA16Float, 64, 64, 0, 0, WMTLoadActionLoad,
                        TraceTextureType(WMTTextureType3D)) == "c0 RGBA16Float 64x64 3d mip 0 slice 0 load");
  CHECK(std::string(TraceTextureType(WMTTextureType2DArray)) == "2d-array");
  CHECK(std::string(TraceTextureType(WMTTextureTypeCube)) == "cube");
  CHECK(std::string(TraceTextureType(WMTTextureTypeCubeArray)) == "cube");
  CHECK(std::string(TraceTextureType(WMTTextureType2D)) == "2d");
  CHECK(TraceRenderPass(7, {"c0 RGBA16Float 64x64 3d mip 0 slice 0 load"}, 1, {"0,0 64x64"}, {}, false, 64) ==
        "pass 7: c0 RGBA16Float 64x64 3d mip 0 slice 0 load; layers 64; 1 draw; viewports 0,0 64x64");
  CHECK(TraceRenderPass(8, {"c0 RGBA16Float 1280x720 2d mip 0 slice 0 load"}, 3, {}, {}, false, 0) ==
        "pass 8: c0 RGBA16Float 1280x720 2d mip 0 slice 0 load; layers 0; 3 draws");

  // a plain draw
  TraceDrawArgs draw;
  draw.count = 3;
  draw.vs_hash = 0x1a2b3c4d;
  CHECK(TraceDrawLine(draw) ==
        "draw vertices 3 start 0 base - instances 1 start instance 0; vs 1a2b3c4d writes -; gs -");
  // indexed and instanced, the vertex shader writing the render-target array index
  TraceDrawArgs volume;
  volume.kind = "draw indexed instanced";
  volume.indexed = true;
  volume.count = 6;
  volume.start = 12;
  volume.base = -4;
  volume.instances = 64;
  volume.start_instance = 2;
  volume.vs_hash = 0xabcdef01;
  volume.vs_writes = 1;
  CHECK(TraceDrawLine(volume) == "draw indexed instanced indices 6 start 12 base -4 instances 64 start instance 2; "
                                 "vs abcdef01 writes rtindex; gs -");
  // a geometry shader, folded or emulated, writing both indices; an unknown vertex signature
  volume.vs_writes = -1;
  volume.gs = true;
  volume.gs_hash = 0x00000abc;
  volume.gs_writes = 3;
  volume.gs_folded = true;
  CHECK(TraceDrawLine(volume) == "draw indexed instanced indices 6 start 12 base -4 instances 64 start instance 2; "
                                 "vs abcdef01 writes ?; gs 00000abc writes rtindex,vpindex folded");
  volume.gs_folded = false;
  volume.gs_writes = 2;
  CHECK(TraceDrawLine(volume).find("; gs 00000abc writes vpindex emulated") != std::string::npos);
  // indirect: the number, or "#?" until the encoder fills it in
  TraceDrawArgs indirect;
  indirect.kind = "draw instanced";
  indirect.indirect = true;
  indirect.vs_hash = 0x11111111;
  CHECK(TraceDrawLine(indirect) == "draw instanced indirect #?; vs 11111111 writes -; gs -");
  indirect.indirect_k = 5;
  CHECK(TraceDrawLine(indirect) == "draw instanced indirect #5; vs 11111111 writes -; gs -");
  CHECK(TraceWritesText(0) == "-");
  CHECK(TraceWritesText(3) == "rtindex,vpindex");
}

void
TestSummaryLine() {
  std::string line = FrameTraceSummary(1800, 2425, 160, 141, 3010, "C:\\users\\Public\\pg-dumps\\frame-1800");
  CHECK(line == "dxmt-frame 1800: 2425 lines, 160 render passes, 141 dispatches, 3010 draws; trace in "
                "C:\\users\\Public\\pg-dumps\\frame-1800\\trace.txt");
  // the one log line per traced frame stays within the 900-byte log cap with the logger's prefix
  CHECK(line.size() <= kFrameTraceMaxLineBytes);
  CHECK(FormatLogLine("info:  ", line).size() <= kLogMessageMaxBytes);
  CHECK(StartsWith(line, "dxmt-frame 1800: "));
}

} // namespace

int
main() {
  TestSwitchAndFrames();
  TestFormats();
  TestFrameAndCap();
  TestTraceFile();
  TestDrawsAndLayers();
  TestSummaryLine();
  return TestSummary("test_frame_trace");
}
