// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 5: the failed-call lines of the Direct3D 9 frontend
// (dxmt/src/d3d9/d3d9_fail_log.hpp): the line's format for a create, a lock (rect and box), a
// level and a null out-pointer; names of results and formats (a FOURCC, an unknown number); the
// expected results not logged; once per (method, result); at most 64 lines.

#include "test_common.hpp"

#include "d3d9_fail_log.hpp"

#include <string>

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
  auto args = [] { return std::string("x"); };
  CHECK(Consider(limiter, 1, "IDirect3D9::CheckDeviceFormat", kNotAvailableHr, false, args).empty());
  CHECK(Consider(limiter, 2, "IDirect3DQuery9::GetData", kStillDrawing, false, args).empty());
  CHECK(Consider(limiter, 3, "IDirect3DQuery9::GetData", 1, false, args).empty());
  CHECK(limiter.used() == 0);
  // a null out-pointer is logged even with an otherwise expected result
  CHECK(!Consider(limiter, 4, "IDirect3DTexture9::LockRect", 0, true, args).empty());
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
  CHECK(!Consider(limiter, 7, "IDirect3DTexture9::LockRect", kInvalidCall, false, args).empty());
  CHECK(Consider(limiter, 7, "IDirect3DTexture9::LockRect", kInvalidCall, false, args).empty());
  CHECK(!Consider(limiter, 7, "IDirect3DTexture9::LockRect", kOutOfVideoMemory, false, args).empty());
  CHECK(!Consider(limiter, 7, "IDirect3DTexture9::LockRect", 0, true, args).empty()); // its own result
  CHECK(!Consider(limiter, 8, "IDirect3DSurface9::LockRect", kInvalidCall, false, args).empty());
  CHECK_EQ(args_built, 4); // the arguments are built only for a line written
  // at most 64 lines in all
  unsigned written = limiter.used();
  for (unsigned m = 100; m < 200; m++)
    if (!Consider(limiter, m, "IDirect3DDevice9::CreateTexture", kInvalidCall, false, args).empty())
      written++;
  CHECK_EQ(written, kMaxLines);
  CHECK_EQ(limiter.used(), kMaxLines);
  CHECK(Consider(limiter, 9, "IDirect3DVolume9::LockBox", kInvalidCall, false, args).empty());
}

} // namespace

int
main() {
  TestFormat();
  TestExpected();
  TestLimits();
  return TestSummary("test_d3d9_fail_log");
}
