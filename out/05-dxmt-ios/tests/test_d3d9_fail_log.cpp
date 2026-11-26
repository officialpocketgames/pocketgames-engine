// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 5: the failed-call lines of the Direct3D 9 frontend
// (dxmt/src/d3d9/d3d9_fail_log.hpp): the line's format for a create, a lock (rect and box), a
// level and a null out-pointer; names of results and formats (a FOURCC, an unknown number); the
// expected results not logged; once per (method, result); at most 64 lines.
// Spec 07 clarification 7: the texture detail of the level and lock lines (size, level count,
// format, usage, pool, the level asked for); a pair that fails again counted, with its last
// arguments, and its line every 10 s; the creates the same way.

#include "test_common.hpp"

#include "d3d9_fail_log.hpp"

#include <string>
#include <vector>

namespace {

using namespace dxmt::d9fail;

constexpr int32_t kInvalidCall = int32_t(0x8876086Cu);
constexpr int32_t kOutOfVideoMemory = int32_t(0x8876017Cu);
constexpr int32_t kNotAvailableHr = int32_t(0x8876086Au);
constexpr int32_t kStillDrawing = int32_t(0x8876021Cu);

struct Rect {
  long left, top, right, bottom;
};
struct Box {
  unsigned Left, Top, Right, Bottom, Front, Back;
};

void
TestFormat() {
  CHECK(Line("IDirect3DDevice9::CreateTexture", kOutOfVideoMemory, false,
             CreateArgs(2048, 1024, 0, 12, 0x200, 21, 0)) ==
        "[d3d9] IDirect3DDevice9::CreateTexture failed: D3DERR_OUTOFVIDEOMEMORY; size 2048x1024, levels 12, usage "
        "0x00000200, format A8R8G8B8, pool DEFAULT");
  CHECK(CreateArgs(64, 64, 16, 1, 0, 0x31545844 /* 'DXT1' */, 1) ==
        "size 64x64x16, levels 1, usage 0x00000000, format DXT1, pool MANAGED");
  Rect rect{8, 16, 72, 80};
  CHECK(LockArgs(2, false, RegionOfRect(&rect), 0x2000, ResourceArgs(66051 /* bytes 3,2,1,0: not a FOURCC */, 2, 0)) ==
        "level 2, rect 8,16-72,80, flags 0x00002000, format 66051, pool SYSTEMMEM, usage 0x00000000");
  CHECK(LockArgs(-1, false, RegionOfRect<Rect>(nullptr), 0, ResourceArgs(80, 0, 2)) ==
        "rect whole, flags 0x00000000, format D16, pool DEFAULT, usage 0x00000002");
  Box box{1, 2, 3, 4, 5, 6};
  CHECK(LockArgs(0, true, RegionOfBox(&box), 0x10, ResourceArgs(113, 3, 0)) ==
        "level 0, box 1,2,5-3,4,6, flags 0x00000010, format A16B16G16R16F, pool SCRATCH, usage 0x00000000");
  // a success with a null out-pointer
  CHECK(Line("IDirect3DTexture9::LockRect", 0, true, "level 0") ==
        "[d3d9] IDirect3DTexture9::LockRect failed: S_OK with a null out-pointer; level 0");
  CHECK(Line("IDirect3DTexture9::GetSurfaceLevel", kInvalidCall, false, "level 9") ==
        "[d3d9] IDirect3DTexture9::GetSurfaceLevel failed: D3DERR_INVALIDCALL; level 9");
  CHECK(HResultName(int32_t(0x887600FFu)) == "0x887600FF");
  CHECK(FormatName(12345) == "12345");
  CHECK(PoolName(9) == "9");
}

void
TestExpected() {
  CHECK(Expected("IDirect3D9::CheckDeviceFormat", kNotAvailableHr));
  CHECK(Expected("IDirect3D9Ex::CheckDeviceMultiSampleType", kNotAvailableHr));
  CHECK(!Expected("IDirect3DDevice9::CreateTexture", kNotAvailableHr)); // only from Check*
  CHECK(Expected("IDirect3DQuery9::GetData", kStillDrawing));
  CHECK(Expected("IDirect3DQuery9::GetData", 1 /* S_FALSE */));
  CHECK(Expected("IDirect3DTexture9::LockRect", 0));
  CHECK(!Expected("IDirect3DTexture9::LockRect", kInvalidCall));

  Limiter limiter;
  int args_built = 0;
  auto args = [&] {
    args_built++;
    return std::string("x");
  };
  for (int i = 0; i < 3; i++) {
    CHECK(Consider(limiter, 1, "IDirect3D9::CheckDeviceFormat", kNotAvailableHr, false, args, 1).empty());
    CHECK(Consider(limiter, 2, "IDirect3DQuery9::GetData", kStillDrawing, false, args, 1).empty());
    CHECK(Consider(limiter, 3, "IDirect3DQuery9::GetData", 1, false, args, 1).empty());
  }
  CHECK(limiter.used() == 0);
  CHECK_EQ(args_built, 0); // nor their arguments built
  CHECK(!limiter.pending()); // nor counted as repeats (clarification 7)
  // a null out-pointer is logged even with an otherwise expected result
  CHECK(!Consider(limiter, 4, "IDirect3DTexture9::LockRect", 0, true, args, 1).empty());
}

void
TestLimits() {
  Limiter limiter;
  int args_built = 0;
  auto args = [&] {
    args_built++;
    return std::string("level 0");
  };
  // once per (method, result)
  CHECK(!Consider(limiter, 7, "IDirect3DTexture9::LockRect", kInvalidCall, false, args, 1).empty());
  CHECK(Consider(limiter, 7, "IDirect3DTexture9::LockRect", kInvalidCall, false, args, 1).empty());
  CHECK(!Consider(limiter, 7, "IDirect3DTexture9::LockRect", kOutOfVideoMemory, false, args, 1).empty());
  CHECK(!Consider(limiter, 7, "IDirect3DTexture9::LockRect", 0, true, args, 1).empty()); // its own result
  CHECK(!Consider(limiter, 8, "IDirect3DSurface9::LockRect", kInvalidCall, false, args, 1).empty());
  CHECK_EQ(args_built, 5); // built for each failure: four lines and the repeat, which keeps them
  // at most 64 lines in all
  unsigned written = limiter.used();
  for (unsigned m = 100; m < 200; m++)
    if (!Consider(limiter, m, "IDirect3DDevice9::CreateTexture", kInvalidCall, false, args, 1).empty())
      written++;
  CHECK_EQ(written, kMaxLines);
  CHECK_EQ(limiter.used(), kMaxLines);
  CHECK(Consider(limiter, 9, "IDirect3DVolume9::LockBox", kInvalidCall, false, args, 1).empty());
}

/* Clarification 7: the texture a level or lock call was on. */
void
TestTextureDetail() {
  constexpr uint32_t kDxt1 = 0x31545844; // 'DXT1'
  constexpr uint32_t kDxt5 = 0x35545844; // 'DXT5'
  // each field: the size, the level count, format, usage and pool
  CHECK(TextureArgs("texture", 256, 128, 0, 9, kDxt1, 0, 1) ==
        "texture 256x128, 9 levels, format DXT1, usage 0x00000000, pool MANAGED");
  CHECK(TextureArgs("cube texture", 64, 64, 0, 1, 21, 0x200, 0) ==
        "cube texture 64x64, 1 level, format A8R8G8B8, usage 0x00000200, pool DEFAULT");
  CHECK(TextureArgs("volume texture", 32, 16, 8, 4, 113, 0x8, 2) ==
        "volume texture 32x16x8, 4 levels, format A16B16G16R16F, usage 0x00000008, pool SYSTEMMEM");
  // a surface or volume of its own: no level count
  CHECK(TextureArgs("surface", 640, 480, 0, -1, 22, 0x1, 0) ==
        "surface 640x480, format X8R8G8B8, usage 0x00000001, pool DEFAULT");
  CHECK(TextureArgs("volume", 16, 16, 16, -1, 50, 0, 3) == "volume 16x16x16, format L8, usage 0x00000000, pool SCRATCH");

  // the lines as the four methods write them: the level asked for, then the texture
  CHECK(Line("IDirect3DTexture9::GetSurfaceLevel", kInvalidCall, false,
             "level 9, " + TextureArgs("texture", 256, 128, 0, 9, kDxt1, 0, 1)) ==
        "[d3d9] IDirect3DTexture9::GetSurfaceLevel failed: D3DERR_INVALIDCALL; level 9, texture 256x128, 9 levels, "
        "format DXT1, usage 0x00000000, pool MANAGED");
  CHECK(Line("IDirect3DCubeTexture9::GetCubeMapSurface", kInvalidCall, false,
             "face 6, level 0, " + TextureArgs("cube texture", 512, 512, 0, 10, kDxt5, 0x400, 0)) ==
        "[d3d9] IDirect3DCubeTexture9::GetCubeMapSurface failed: D3DERR_INVALIDCALL; face 6, level 0, cube texture "
        "512x512, 10 levels, format DXT5, usage 0x00000400, pool DEFAULT");
  Rect rect{0, 0, 64, 64};
  CHECK(Line("IDirect3DTexture9::LockRect", 0, true,
             LockArgs(3, false, RegionOfRect(&rect), 0x10,
                      TextureArgs("texture", 1024, 1024, 0, 11, 21, 0x200, 0))) ==
        "[d3d9] IDirect3DTexture9::LockRect failed: S_OK with a null out-pointer; level 3, rect 0,0-64,64, flags "
        "0x00000010, texture 1024x1024, 11 levels, format A8R8G8B8, usage 0x00000200, pool DEFAULT");
  CHECK(LockArgs(1, true, RegionOfBox<Box>(nullptr), 0, TextureArgs("volume texture", 32, 32, 32, 6, 50, 0, 1)) ==
        "level 1, box whole, flags 0x00000000, volume texture 32x32x32, 6 levels, format L8, usage 0x00000000, "
        "pool MANAGED");
}

/* Clarification 7: a pair that fails again is counted; every 10 s, one line per pair that did. */
void
TestRepeats() {
  constexpr uint64_t s = 1'000'000'000ull;
  const char *surface_level = "IDirect3DTexture9::GetSurfaceLevel";
  const char *lock_rect = "IDirect3DTexture9::LockRect";
  const char *create = "IDirect3DDevice9::CreateTexture";
  Limiter limiter;
  std::string level = "level 0";
  auto args = [&] { return level; };

  // the first failure is the line of clarification 5; nothing waits yet
  CHECK(Consider(limiter, 1, surface_level, kInvalidCall, false, args, 1 * s) ==
        "[d3d9] IDirect3DTexture9::GetSurfaceLevel failed: D3DERR_INVALIDCALL; level 0");
  CHECK(!limiter.pending());
  CHECK(limiter.repeatLines(100 * s).empty());

  // three repeats, from 2 s: counted, no line until 10 s after the first of them
  for (int i = 1; i <= 3; i++) {
    level = "level " + std::to_string(8 + i);
    CHECK(Consider(limiter, 1, surface_level, kInvalidCall, false, args, uint64_t(1 + i) * s).empty());
  }
  CHECK(limiter.pending());
  CHECK(limiter.repeatLines(5 * s).empty());
  CHECK(limiter.repeatLines(12 * s - 1).empty());
  CHECK(limiter.pending());
  std::vector<std::string> lines = limiter.repeatLines(12 * s);
  CHECK_EQ(lines.size(), 1u);
  if (lines.size() == 1)
    CHECK(lines[0] == "[d3d9] IDirect3DTexture9::GetSurfaceLevel -> D3DERR_INVALIDCALL failed 3 more times (last: "
                      "level 11)");
  // the counts start over
  CHECK(!limiter.pending());
  CHECK(limiter.repeatLines(30 * s).empty());

  // several pairs: a line for each that failed again, in the order they were first logged;
  // the null-out result and another result of the same method are pairs of their own
  level = "level 2, rect whole";
  CHECK(!Consider(limiter, 2, lock_rect, kInvalidCall, false, args, 40 * s).empty());
  CHECK(!Consider(limiter, 2, lock_rect, 0, true, args, 40 * s).empty());
  CHECK(!Consider(limiter, 3, create, kOutOfVideoMemory, false, args, 40 * s).empty());
  CHECK(!Consider(limiter, 3, create, 0, true, args, 40 * s).empty()); // a create with a null out-pointer
  CHECK(!limiter.pending());
  level = "size 4096x4096, levels 1, usage 0x00000001, format A8R8G8B8, pool DEFAULT";
  CHECK(Consider(limiter, 3, create, 0, true, args, 41 * s).empty());
  CHECK(Consider(limiter, 3, create, 0, true, args, 42 * s).empty());
  level = "level 4, rect whole";
  CHECK(Consider(limiter, 2, lock_rect, 0, true, args, 43 * s).empty());
  level = "level 12";
  CHECK(Consider(limiter, 1, surface_level, kInvalidCall, false, args, 44 * s).empty());
  CHECK(limiter.repeatLines(50 * s).empty()); // due at 51 s: 10 s after the first repeat
  lines = limiter.repeatLines(51 * s);
  CHECK_EQ(lines.size(), 3u);
  if (lines.size() == 3) {
    CHECK(lines[0] == "[d3d9] IDirect3DTexture9::GetSurfaceLevel -> D3DERR_INVALIDCALL failed 1 more times (last: "
                      "level 12)");
    CHECK(lines[1] == "[d3d9] IDirect3DTexture9::LockRect -> S_OK with a null out-pointer failed 1 more times "
                      "(last: level 4, rect whole)");
    CHECK(lines[2] == "[d3d9] IDirect3DDevice9::CreateTexture -> S_OK with a null out-pointer failed 2 more times "
                      "(last: size 4096x4096, levels 1, usage 0x00000001, format A8R8G8B8, pool DEFAULT)");
  }

  // the period starts again with the next repeat, not at the last report
  level = "size 2048x2048, levels 0, usage 0x00000000, format DXT5, pool MANAGED";
  CHECK(Consider(limiter, 3, create, kOutOfVideoMemory, false, args, 100 * s).empty());
  CHECK(limiter.repeatLines(109 * s).empty());
  lines = limiter.repeatLines(110 * s);
  CHECK_EQ(lines.size(), 1u);
  if (lines.size() == 1)
    CHECK(lines[0] == "[d3d9] IDirect3DDevice9::CreateTexture -> D3DERR_OUTOFVIDEOMEMORY failed 1 more times (last: "
                      "size 2048x2048, levels 0, usage 0x00000000, format DXT5, pool MANAGED)");

  // a pair past the 64 is neither logged nor counted; one already logged still is
  Limiter full;
  for (unsigned m = 0; m < kMaxLines; m++)
    CHECK(!Consider(full, 100 + m, create, kInvalidCall, false, args, 1 * s).empty());
  CHECK(Consider(full, 500, lock_rect, kInvalidCall, false, args, 2 * s).empty());
  CHECK(Consider(full, 500, lock_rect, kInvalidCall, false, args, 3 * s).empty());
  CHECK(!full.pending());
  CHECK(Consider(full, 100, create, kInvalidCall, false, args, 4 * s).empty());
  CHECK(full.pending());
  lines = full.repeatLines(14 * s);
  CHECK_EQ(lines.size(), 1u);
  if (lines.size() == 1)
    CHECK(lines[0].rfind("[d3d9] IDirect3DDevice9::CreateTexture -> D3DERR_INVALIDCALL failed 1 more times", 0) == 0);

  // many repeats in one period: one line, the count, the last arguments
  Limiter busy;
  level = "level 0";
  CHECK(!Consider(busy, 1, surface_level, kInvalidCall, false, args, 1 * s).empty());
  for (int i = 0; i < 5000; i++) {
    level = "level " + std::to_string(i);
    Consider(busy, 1, surface_level, kInvalidCall, false, args, 2 * s + uint64_t(i) * 1000);
  }
  lines = busy.repeatLines(12 * s);
  CHECK_EQ(lines.size(), 1u);
  if (lines.size() == 1)
    CHECK(lines[0] == "[d3d9] IDirect3DTexture9::GetSurfaceLevel -> D3DERR_INVALIDCALL failed 5000 more times "
                      "(last: level 4999)");
}

} // namespace

int
main() {
  TestFormat();
  TestExpected();
  TestLimits();
  TestTextureDetail();
  TestRepeats();
  return TestSummary("test_d3d9_fail_log");
}
