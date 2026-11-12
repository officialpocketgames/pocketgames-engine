// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarifications 28, 31 and 33-37: the frame dump's plain part
// (dxmt/src/dxmt/dxmt_frame_dump.hpp): when it is on (both switches, presents 1800 and 9000),
// which formats and types are dumped and their texel sizes, subresource and present names, the
// one-buffer-per-subresource accounting with first-write copies, and the index's lines and
// reasons. No image is made or read here: the dump only runs on a device, in test builds.

#include "test_common.hpp"

#include "dxmt_frame_dump.hpp"

#include <string>

namespace {

using namespace dxmt;

void
TestPresents() {
  // clarification 36: presents 1800 and 9000, both traced presents too; off unless both switches
  CHECK(FrameDumpActive(true, true, 1800));
  CHECK(FrameDumpActive(true, true, 9000));
  CHECK(IsTracedPresent(1800) && IsTracedPresent(9000));
  CHECK(!FrameDumpActive(false, true, 1800)); // the dump switch unset: never
  CHECK(!FrameDumpActive(false, true, 9000));
  CHECK(!FrameDumpActive(true, false, 9000)); // the trace off: never
  CHECK(!FrameDumpActive(true, true, 600));
  CHECK(!FrameDumpActive(true, true, 3000));
  CHECK(!FrameDumpActive(true, true, 10200));
  CHECK(!FrameDumpActive(true, true, 0));
  CHECK(!FrameTraceSwitchValue("")); // PG_DXMT_FRAME_DUMP unset reads ""
  CHECK(FrameDumpDirectory(1800) == "C:\\users\\Public\\pg-dumps\\frame-1800");
  CHECK(FrameDumpDirectory(9000) == "C:\\users\\Public\\pg-dumps\\frame-9000");
}

void
TestFormats() {
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA16Float), 8);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA8Unorm_sRGB), 4);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRG11B10Float), 4);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatR8Snorm), 1);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatRGBA32Float), 16);
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatBGRX8Unorm), 4); // swizzle flag dropped
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatR32Uint), 0);    // integer: "format"
  CHECK_EQ(FrameDumpBytesPerTexel(WMTPixelFormatBC1_RGBA), 0);

  // depth (clarifications 31 and 34)
  FrameDumpPlane planes[2];
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatDepth32Float, planes), 1);
  CHECK_EQ(planes[0].bytes_per_texel, 4);
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatDepth16Unorm, planes), 1);
  CHECK(std::string(planes[0].format_label) == "Depth16Unorm");
  CHECK_EQ(planes[0].bytes_per_texel, 2);
  CHECK_EQ(planes[0].option, WMTBlitOptionNone);
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatDepth32Float_Stencil8, planes), 2);
  CHECK_EQ(planes[0].option, WMTBlitOptionDepthFromDepthStencil);
  CHECK(std::string(planes[1].format_label) == "Stencil8");
  CHECK_EQ(planes[1].bytes_per_texel, 1);
  CHECK_EQ(planes[1].option, WMTBlitOptionStencilFromDepthStencil);
  CHECK_EQ(FrameDumpDepthPlanes(WMTPixelFormatDepth24Unorm_Stencil8, planes), 0);

  // types (clarification 34)
  CHECK(FrameDumpLayersOf(WMTTextureType2D) == FrameDumpLayers::Slice);
  CHECK(FrameDumpLayersOf(WMTTextureType2DArray) == FrameDumpLayers::Slice);
  CHECK(FrameDumpLayersOf(WMTTextureTypeCube) == FrameDumpLayers::Face);
  CHECK(FrameDumpLayersOf(WMTTextureTypeCubeArray) == FrameDumpLayers::Face);
  CHECK(FrameDumpLayersOf(WMTTextureType3D) == FrameDumpLayers::Z0);
  CHECK(FrameDumpLayersOf(WMTTextureType2DMultisample) == FrameDumpLayers::NotDumped);
  CHECK(FrameDumpLayersOf(WMTTextureType2DMultisampleArray) == FrameDumpLayers::NotDumped);
  CHECK(FrameDumpLayersOf(WMTTextureTypeTextureBuffer) == FrameDumpLayers::NotDumped);
}

void
TestNames() {
  CHECK(FrameDumpLayerName(FrameDumpLayers::Slice, false, 0, 0) == "mip0-slice0");
  CHECK(FrameDumpLayerName(FrameDumpLayers::Slice, false, 2, 5) == "mip2-slice5"); // an array slice
  CHECK(FrameDumpLayerName(FrameDumpLayers::Face, false, 0, 3) == "mip0-face3");   // a cube face
  CHECK(FrameDumpLayerName(FrameDumpLayers::Face, true, 1, 8) == "mip1-slice1-face2"); // a cube array's layer 8
  CHECK(FrameDumpLayerName(FrameDumpLayers::Z0, false, 0, 7) == "mip0-z0");
  CHECK(FrameDumpSubresourceName(7, "RGBA16Float", 1280, 720, "mip0-slice0", false) ==
        "t0007-RGBA16Float-1280x720-mip0-slice0.raw");
  CHECK(FrameDumpSubresourceName(7, "RGBA16Float", 1280, 720, "mip0-slice0", true) ==
        "t0007-RGBA16Float-1280x720-mip0-slice0-first.raw");
  CHECK(FrameDumpSubresourceName(12, "Stencil8", 1280, 720, "mip0-slice0", false) ==
        "t0012-Stencil8-1280x720-mip0-slice0.raw");
  CHECK(FrameDumpPresentName(WMTPixelFormatBGRA8Unorm, 1280, 720) == "present-BGRA8Unorm-1280x720.raw");
  CHECK(FrameDumpTextureId(0) == "t0000");
  CHECK(FrameDumpDispatchTag(0x7041a1e5) == "cs7041a1e5");
}

void
TestOneBufferPerSubresource() {
  FrameDumpLedger ledger;
  FrameDumpKey depth{0x10, 0, 0, 0}, stencil{0x10, 0, 0, 1}, color{0x20, 0, 0, 0};
  uint64_t depth_bytes = 1280ull * 720 * 4, stencil_bytes = 1280ull * 720, color_bytes = 1280ull * 720 * 8;
  // build 66's case: the same depth and stencil planes written in 25 passes
  CHECK(ledger.write(depth, depth_bytes) == FrameDumpAction::CopyNew);
  CHECK(ledger.write(stencil, stencil_bytes) == FrameDumpAction::CopyNew);
  CHECK(ledger.write(depth, depth_bytes) == FrameDumpAction::CopyWithFirst); // second write: first copy
  CHECK(ledger.write(stencil, stencil_bytes) == FrameDumpAction::CopyWithFirst);
  for (int pass = 2; pass < 25; pass++) {
    CHECK(ledger.write(depth, depth_bytes) == FrameDumpAction::Copy); // rewrites cost nothing
    CHECK(ledger.write(stencil, stencil_bytes) == FrameDumpAction::Copy);
  }
  CHECK_EQ(ledger.lastBytes(), depth_bytes + stencil_bytes); // counted once each
  CHECK_EQ(ledger.firstBytes(), depth_bytes + stencil_bytes);
  CHECK_EQ(ledger.files(), 2);
  CHECK_EQ(ledger.firstFiles(), 2);
  // later passes still get their buffers
  CHECK(ledger.write(color, color_bytes) == FrameDumpAction::CopyNew);
  // other mips, slices and planes are other subresources
  CHECK(ledger.write({0x20, 1, 0, 0}, color_bytes / 4) == FrameDumpAction::CopyNew);
  CHECK(ledger.write({0x20, 0, 1, 0}, color_bytes) == FrameDumpAction::CopyNew);
}

void
TestBudget() {
  // 16 MB per subresource: a 2048x2048 Depth32Float is exactly 16 MB and fits
  FrameDumpLedger ledger;
  CHECK(ledger.write({1, 0, 0, 0}, 2048ull * 2048 * 4) == FrameDumpAction::CopyNew);
  CHECK(ledger.write({2, 0, 0, 0}, (16ull << 20) + 1) == FrameDumpAction::Skip);
  CHECK(ledger.write({2, 0, 0, 0}, (16ull << 20) + 1) == FrameDumpAction::Skip); // stays refused
  CHECK_EQ(ledger.refused(), 1);

  // the 256 MB total: last-state buffers fill it; then subresources are refused
  FrameDumpLedger full;
  for (uint64_t t = 0; t < 16; t++)
    CHECK(full.write({100 + t, 0, 0, 0}, 16ull << 20) == FrameDumpAction::CopyNew);
  CHECK_EQ(full.lastBytes(), 256ull << 20);
  CHECK(full.write({200, 0, 0, 0}, 4) == FrameDumpAction::Skip);
  // no room left: a second write takes no first copy, but still copies into its buffer
  CHECK(full.write({100, 0, 0, 0}, 16ull << 20) == FrameDumpAction::Copy);
  CHECK_EQ(full.firstFiles(), 0);

  // first-write copies take at most 64 MB, whatever is left
  FrameDumpLedger share;
  for (uint64_t t = 0; t < 6; t++)
    CHECK(share.write({300 + t, 0, 0, 0}, 16ull << 20) == FrameDumpAction::CopyNew);
  for (uint64_t t = 0; t < 4; t++)
    CHECK(share.write({300 + t, 0, 0, 0}, 16ull << 20) == FrameDumpAction::CopyWithFirst);
  CHECK(share.write({304, 0, 0, 0}, 16ull << 20) == FrameDumpAction::Copy); // the share is used up
  CHECK_EQ(share.firstBytes(), 64ull << 20);
  // and last-state buffers still fit in what remains: 96 + 64 = 160 MB used, 96 MB left
  for (uint64_t t = 0; t < 6; t++)
    CHECK(share.write({400 + t, 0, 0, 0}, 16ull << 20) == FrameDumpAction::CopyNew);
  CHECK(share.write({500, 0, 0, 0}, 1) == FrameDumpAction::Skip);
  CHECK(share.summary() == "dump: 12 files (4 with a first-write copy), 256.0 MB; 1 subresources left out (budget)");
}

void
TestSrvReadbackLimit() {
  // clarification 50: at most 300 vertex-shader buffer read-backs a frame
  FrameDumpLedger ledger;
  for (unsigned i = 0; i < kFrameTraceMaxSrvReadbacks; i++)
    CHECK(ledger.takeSrvReadback(256));
  CHECK(!ledger.takeSrvReadback(256));
  CHECK(!ledger.takeSrvReadback(256));
  CHECK_EQ(ledger.srvReadbacks(), 300u);
  CHECK_EQ(ledger.srvSkipped(), 2u);
  CHECK_EQ(ledger.firstBytes(), 300ull * 256);

  // ... out of item 41's 64 MB first-write share: none once the share is used up
  FrameDumpLedger used;
  for (uint64_t t = 0; t < 4; t++) {
    CHECK(used.write({300 + t, 0, 0, 0}, 16ull << 20) == FrameDumpAction::CopyNew);
    CHECK(used.write({300 + t, 0, 0, 0}, 16ull << 20) == FrameDumpAction::CopyWithFirst);
  }
  CHECK(!used.takeSrvReadback(256));
  CHECK_EQ(used.srvReadbacks(), 0u);
  CHECK_EQ(used.srvSkipped(), 1u);
  // and the share's last bytes still go
  FrameDumpLedger nearly;
  CHECK(nearly.takeShare((64ull << 20) - 256));
  CHECK(nearly.takeSrvReadback(256));
  CHECK(!nearly.takeSrvReadback(256));
  CHECK_EQ(nearly.firstBytes(), 64ull << 20);
  // a new frame's ledger starts again
  CHECK((FrameDumpLedger{}).takeSrvReadback(256));

  // clarification 58: constant-buffer copies, 300 a frame counted apart from the views'
  FrameDumpLedger both;
  for (unsigned i = 0; i < kFrameTraceMaxSrvReadbacks; i++)
    CHECK(both.takeSrvReadback(256));
  CHECK(!both.takeSrvReadback(256));
  for (unsigned i = 0; i < kFrameTraceMaxCbReadbacks; i++)
    CHECK(both.takeCbReadback(4096));
  CHECK(!both.takeCbReadback(16));
  CHECK_EQ(both.cbReadbacks(), 300u);
  CHECK_EQ(both.cbSkipped(), 1u);
  CHECK_EQ(both.srvSkipped(), 1u);
  CHECK_EQ(both.firstBytes(), 300ull * 256 + 300ull * 4096);
  // and out of the same 64 MB share
  FrameDumpLedger tight;
  CHECK(tight.takeShare((64ull << 20) - 4096));
  CHECK(tight.takeCbReadback(4096));
  CHECK(!tight.takeCbReadback(16));
  CHECK(!tight.takeSrvReadback(256));
  CHECK_EQ(tight.cbSkipped(), 1u);
}

void
TestIndex() {
  CHECK(std::string(FrameDumpReasonText(FrameDumpReason::Format)) == "format");
  CHECK(std::string(FrameDumpReasonText(FrameDumpReason::Type)) == "type");
  CHECK(std::string(FrameDumpReasonText(FrameDumpReason::NotStored)) == "not stored");
  CHECK(std::string(FrameDumpReasonText(FrameDumpReason::Budget)) == "budget");
  CHECK(FrameDumpIndexLine("pass 14", "c0", 7, "RGBA16Float", WMTTextureType2D, FrameDumpSizeText(1280, 720),
                           "mip0-slice0", FrameDumpReason::Dumped, "t0007-RGBA16Float-1280x720-mip0-slice0.raw") ==
        "pass 14 c0: t0007 RGBA16Float 2D 1280x720 mip0-slice0 -> t0007-RGBA16Float-1280x720-mip0-slice0.raw");
  CHECK(FrameDumpIndexLine("pass 3", "c1", 2, "RGBA16Float", WMTTextureType2DMultisample, FrameDumpSizeText(1280, 720),
                           "mip0-slice0", FrameDumpReason::Type, "") ==
        "pass 3 c1: t0002 RGBA16Float 2DMS 1280x720 mip0-slice0 not dumped (type)");
  CHECK(FrameDumpIndexLine("dispatch #4 cs7041a1e5", "u0", 5, "R32Uint", WMTTextureType2D, FrameDumpSizeText(640, 360),
                           "mip0-slice0", FrameDumpReason::Format, "") ==
        "dispatch #4 cs7041a1e5 u0: t0005 R32Uint 2D 640x360 mip0-slice0 not dumped (format)");
  CHECK(FrameDumpIndexLine("pass 9", "depth", 3, "Depth32Float", WMTTextureType2D, FrameDumpSizeText(2048, 2048),
                           "mip0-slice0", FrameDumpReason::NotStored, "") ==
        "pass 9 depth: t0003 Depth32Float 2D 2048x2048 mip0-slice0 not dumped (not stored)");
  CHECK(FrameDumpIndexLine("copy", "", 9, "RGBA8Unorm", WMTTextureType2D, FrameDumpSizeText(512, 512), "mip0-slice0",
                           FrameDumpReason::Budget, "") ==
        "copy: t0009 RGBA8Unorm 2D 512x512 mip0-slice0 not dumped (budget)");
  // clarification 42: a whole 3D mip, and a fallback slice with its note
  CHECK(FrameDumpIndexLine("pass 2", "c0", 1, "RGBA16Float", WMTTextureType3D, FrameDumpSizeText(64, 64, 64),
                           "mip0-3d", FrameDumpReason::Dumped, "t0001-RGBA16Float-64x64x64-mip0-3d.raw") ==
        "pass 2 c0: t0001 RGBA16Float 3D 64x64x64 mip0-3d -> t0001-RGBA16Float-64x64x64-mip0-3d.raw");
  CHECK(FrameDumpIndexLine("pass 5", "c0", 4, "RGBA16Float", WMTTextureType3D, FrameDumpSizeText(512, 512, 64),
                           "mip0-z32 (slices 0, 32, 63 of 64)", FrameDumpReason::Dumped,
                           "t0004-RGBA16Float-512x512-mip0-z32.raw") ==
        "pass 5 c0: t0004 RGBA16Float 3D 512x512x64 mip0-z32 (slices 0, 32, 63 of 64) -> "
        "t0004-RGBA16Float-512x512-mip0-z32.raw");
}

void
TestVolumes() {
  // clarification 42: a 64^3 RGBA16Float mip is 2 MB: dumped whole, one file
  auto whole = FrameDumpPlan3D(64, 64, 64, 8);
  CHECK(whole.whole);
  CHECK_EQ(64ull * 64 * 64 * 8, 2ull << 20);
  CHECK(FrameDumpVolumeName(7, "RGBA16Float", 64, 64, 64, 0, false) == "t0007-RGBA16Float-64x64x64-mip0-3d.raw");
  CHECK(FrameDumpVolumeName(7, "RGBA16Float", 64, 64, 64, 0, true) == "t0007-RGBA16Float-64x64x64-mip0-3d-first.raw");
  // exactly 16 MB fits: 128^3 RGBA8 is 8 MB, 256x256x64 RGBA8 16 MB
  CHECK(FrameDumpPlan3D(256, 256, 64, 4).whole);
  // larger: slices 0, d/2 and d-1
  auto large = FrameDumpPlan3D(512, 512, 64, 8);
  CHECK(!large.whole);
  CHECK_EQ(large.count, 3);
  CHECK_EQ(large.slices[0], 0);
  CHECK_EQ(large.slices[1], 32);
  CHECK_EQ(large.slices[2], 63);
  // distinct ones only: a 2-deep volume has slices 0 and 1
  auto shallow = FrameDumpPlan3D(4096, 4096, 2, 8);
  CHECK(!shallow.whole);
  CHECK_EQ(shallow.count, 2);
  CHECK_EQ(shallow.slices[0], 0);
  CHECK_EQ(shallow.slices[1], 1);
  CHECK(FrameDumpDepthSliceName(0, 32) == "mip0-z32");
  CHECK(FrameDumpSubresourceName(4, "RGBA16Float", 512, 512, FrameDumpDepthSliceName(0, 63), false) ==
        "t0004-RGBA16Float-512x512-mip0-z63.raw");
  // the budget counts the whole mip once; a 64^3 volume rewritten four times costs 2 MB
  FrameDumpLedger ledger;
  FrameDumpKey volume{0x40, 0, kFrameDumpWholeVolume, 0};
  CHECK(ledger.write(volume, 2ull << 20) == FrameDumpAction::CopyNew);
  CHECK(ledger.write(volume, 2ull << 20) == FrameDumpAction::CopyWithFirst);
  CHECK(ledger.write(volume, 2ull << 20) == FrameDumpAction::Copy);
  CHECK(ledger.write(volume, 2ull << 20) == FrameDumpAction::Copy);
  CHECK_EQ(ledger.lastBytes(), 2ull << 20);
  CHECK(FrameDumpSizeText(64, 64, 64) == "64x64x64");
  CHECK(FrameDumpSizeText(64, 64) == "64x64");
}

/* clarification 40: a fake request file that counts what the policy did with it */
struct FakeRequestFiles : FrameDumpRequestFiles {
  bool present = false;
  bool deletable = true;
  std::string content = "requested 2026-11-11T10:00:00Z";
  unsigned reads = 0, removes = 0;
  bool
  read(std::string &line) override {
    reads++;
    if (!present)
      return false;
    line = content;
    return true;
  }
  bool
  remove() override {
    removes++;
    if (!deletable)
      return false;
    present = false;
    return true;
  }
};

void
TestRequests() {
  // the check interval: only at multiples of 60 presents
  {
    FrameDumpRequestPolicy policy;
    FakeRequestFiles files;
    files.present = true;
    std::string log;
    for (uint64_t p = 1; p < 60; p++)
      CHECK_EQ(policy.check(p, true, false, files, log), 0);
    CHECK_EQ(files.reads, 0);
    // delete, then dump the next present
    CHECK_EQ(policy.check(60, true, false, files, log), 61);
    CHECK_EQ(files.reads, 1);
    CHECK_EQ(files.removes, 1);
    CHECK(!files.present);
    CHECK(log == "dxmt: frame dump requested (requested 2026-11-11T10:00:00Z); dumping present 61");
    // no file: no request
    CHECK_EQ(policy.check(120, true, false, files, log), 0);
    CHECK(log.empty());
  }
  // at most 4 per process
  {
    FrameDumpRequestPolicy policy;
    FakeRequestFiles files;
    std::string log;
    unsigned taken = 0;
    for (uint64_t p = 60; p <= 600; p += 60) {
      files.present = true;
      if (policy.check(p, true, false, files, log))
        taken++;
    }
    CHECK_EQ(taken, 4);
    CHECK_EQ(policy.granted(), 4);
    CHECK_EQ(files.removes, 4); // no longer looked at after the fourth
  }
  // a request during a dump waits for the next check, file left in place
  {
    FrameDumpRequestPolicy policy;
    FakeRequestFiles files;
    files.present = true;
    std::string log;
    CHECK_EQ(policy.check(1800, true, true, files, log), 0);
    CHECK(files.present);
    CHECK_EQ(files.reads, 0);
    CHECK_EQ(policy.check(1860, true, false, files, log), 1861);
  }
  // the switch off: never looked at
  {
    FrameDumpRequestPolicy policy;
    FakeRequestFiles files;
    files.present = true;
    std::string log;
    for (uint64_t p = 60; p <= 6000; p += 60)
      CHECK_EQ(policy.check(p, false, false, files, log), 0);
    CHECK_EQ(files.reads, 0);
    CHECK_EQ(files.removes, 0);
  }
  // a file that can't be deleted: logged once, then no more checks
  {
    FrameDumpRequestPolicy policy;
    FakeRequestFiles files;
    files.present = true;
    files.deletable = false;
    std::string log;
    CHECK_EQ(policy.check(60, true, false, files, log), 0);
    CHECK(log == "dxmt: frame dump request file can't be deleted; no more requests are taken");
    CHECK(policy.stopped());
    CHECK_EQ(policy.check(120, true, false, files, log), 0);
    CHECK(log.empty());
    CHECK_EQ(files.reads, 1);
  }
  // a requested present is dumped (with both switches), like the fixed ones
  CHECK(FrameDumpActive(true, true, 61, true));
  CHECK(!FrameDumpActive(false, true, 61, true));
  CHECK(!FrameDumpActive(true, true, 61, false));
  CHECK(std::string(kFrameDumpRequestFile) == "C:\\users\\Public\\pg-dumps\\frame-request");
}

} // namespace

int
main() {
  TestVolumes();
  TestRequests();
  TestPresents();
  TestFormats();
  TestNames();
  TestOneBufferPerSubresource();
  TestBudget();
  TestSrvReadbackLimit();
  TestIndex();
  return TestSummary("test_frame_dump");
}
