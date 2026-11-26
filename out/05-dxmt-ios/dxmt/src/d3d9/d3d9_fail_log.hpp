// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 5: a failed Direct3D 9 call is logged. When a covered method returns a
// failing HRESULT, or succeeds with a null out-pointer it should have set, one line:
//   [d3d9] <Interface>::<Method> failed: <hresult name or hex>; <arguments>
// once per (method, result) pair, at most 64 lines per process, in every build. Expected results
// aren't failures: D3DERR_NOTAVAILABLE from Check*, D3DERR_WASSTILLDRAWING, and any success
// (S_FALSE included) with its out-pointer set.
//
// Pure: no Windows or Metal types, so the host test (tests/test_d3d9_fail_log.cpp) builds it as
// it is. HRESULTs are 32-bit values, the D3D enums plain numbers (Microsoft's d3d9.h and
// d3d9types.h). The covered methods (d3d9_call_stats.cpp's NoteFailure) wrap their body in
// D9_CHECKED, which pays one branch when the call succeeds.

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace dxmt::d9fail {

constexpr unsigned kMaxLines = 64;

constexpr uint32_t kD3DErr(uint32_t code) {
  return 0x88760000u | code; /* MAKE_D3DHRESULT: severity 1, facility 0x876 */
}
constexpr uint32_t kNotAvailable = kD3DErr(2154);
constexpr uint32_t kWasStillDrawing = kD3DErr(540);

inline bool
Failed(int32_t hr) {
  return hr < 0;
}

/* `name` is "<Interface>::<Method>". */
inline bool
Expected(const char *name, int32_t hr) {
  if (!Failed(hr))
    return true;
  if (uint32_t(hr) == kWasStillDrawing)
    return true;
  if (uint32_t(hr) == kNotAvailable) {
    const char *method = name;
    for (const char *p = name; *p; p++)
      if (p[0] == ':' && p[1] == ':')
        method = p + 2;
    return method[0] == 'C' && method[1] == 'h' && method[2] == 'e' && method[3] == 'c' && method[4] == 'k';
  }
  return false;
}

inline std::string
Hex(uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", value);
  return text;
}

inline std::string
HResultName(int32_t hr) {
  switch (uint32_t(hr)) {
  case 0x8876086Cu: return "D3DERR_INVALIDCALL";
  case 0x8876017Cu: return "D3DERR_OUTOFVIDEOMEMORY";
  case 0x8876086Au: return "D3DERR_NOTAVAILABLE";
  case 0x88760868u: return "D3DERR_DEVICELOST";
  case 0x88760869u: return "D3DERR_DEVICENOTRESET";
  case 0x88760870u: return "D3DERR_DEVICEREMOVED";
  case 0x88760874u: return "D3DERR_DEVICEHUNG";
  case 0x88760827u: return "D3DERR_DRIVERINTERNALERROR";
  case 0x8876086Bu: return "D3DERR_INVALIDDEVICE";
  case 0x8876086Du: return "D3DERR_DRIVERINVALIDCALL";
  case 0x88760866u: return "D3DERR_NOTFOUND";
  case 0x88760867u: return "D3DERR_MOREDATA";
  case 0x88760818u: return "D3DERR_WRONGTEXTUREFORMAT";
  case 0x8876021Cu: return "D3DERR_WASSTILLDRAWING";
  case 0x8007000Eu: return "E_OUTOFMEMORY";
  case 0x80070057u: return "E_INVALIDARG";
  case 0x80004001u: return "E_NOTIMPL";
  case 0x80004002u: return "E_NOINTERFACE";
  case 0x80004003u: return "E_POINTER";
  case 0x80004005u: return "E_FAIL";
  default: return Hex(uint32_t(hr));
  }
}

/* D3DFORMAT: its name without D3DFMT_, a FOURCC as its four characters, else the number */
inline std::string
FormatName(uint32_t format) {
  switch (format) {
  case 0: return "UNKNOWN";
  case 20: return "R8G8B8";
  case 21: return "A8R8G8B8";
  case 22: return "X8R8G8B8";
  case 23: return "R5G6B5";
  case 24: return "X1R5G5B5";
  case 25: return "A1R5G5B5";
  case 26: return "A4R4G4B4";
  case 27: return "R3G3B2";
  case 28: return "A8";
  case 29: return "A8R3G3B2";
  case 30: return "X4R4G4B4";
  case 31: return "A2B10G10R10";
  case 32: return "A8B8G8R8";
  case 33: return "X8B8G8R8";
  case 34: return "G16R16";
  case 35: return "A2R10G10B10";
  case 36: return "A16B16G16R16";
  case 40: return "A8P8";
  case 41: return "P8";
  case 50: return "L8";
  case 51: return "A8L8";
  case 52: return "A4L4";
  case 60: return "V8U8";
  case 61: return "L6V5U5";
  case 62: return "X8L8V8U8";
  case 63: return "Q8W8V8U8";
  case 64: return "V16U16";
  case 67: return "A2W10V10U10";
  case 70: return "D16_LOCKABLE";
  case 71: return "D32";
  case 73: return "D15S1";
  case 75: return "D24S8";
  case 77: return "D24X8";
  case 79: return "D24X4S4";
  case 80: return "D16";
  case 81: return "L16";
  case 82: return "D32F_LOCKABLE";
  case 83: return "D24FS8";
  case 84: return "D32_LOCKABLE";
  case 85: return "S8_LOCKABLE";
  case 100: return "VERTEXDATA";
  case 101: return "INDEX16";
  case 102: return "INDEX32";
  case 110: return "Q16W16V16U16";
  case 111: return "R16F";
  case 112: return "G16R16F";
  case 113: return "A16B16G16R16F";
  case 114: return "R32F";
  case 115: return "G32R32F";
  case 116: return "A32B32G32R32F";
  case 117: return "CxV8U8";
  case 118: return "A1";
  case 119: return "A2B10G10R10_XR_BIAS";
  case 199: return "BINARYBUFFER";
  default: break;
  }
  char c[4] = {char(format & 0xFF), char((format >> 8) & 0xFF), char((format >> 16) & 0xFF), char(format >> 24)};
  bool printable = true;
  for (char ch : c)
    printable &= ch >= 0x20 && ch < 0x7F;
  if (printable)
    return std::string(c, 4);
  return std::to_string(format);
}

inline std::string
PoolName(uint32_t pool) {
  switch (pool) {
  case 0: return "DEFAULT";
  case 1: return "MANAGED";
  case 2: return "SYSTEMMEM";
  case 3: return "SCRATCH";
  default: return std::to_string(pool);
  }
}

/* creates: "size <w>x<h>[x<d>], levels <n>, usage 0x..., format <name>, pool <name>" */
inline std::string
CreateArgs(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format,
           uint32_t pool) {
  std::string s = "size " + std::to_string(width) + "x" + std::to_string(height);
  if (depth)
    s += "x" + std::to_string(depth);
  return s + ", levels " + std::to_string(levels) + ", usage " + Hex(usage) + ", format " + FormatName(format) +
         ", pool " + PoolName(pool);
}

/* the resource a lock was on: "; format <name>, pool <name>, usage 0x..." */
inline std::string
ResourceArgs(uint32_t format, uint32_t pool, uint32_t usage) {
  return "format " + FormatName(format) + ", pool " + PoolName(pool) + ", usage " + Hex(usage);
}

/* a rect or box, copied out of RECT / D3DBOX (whose field types differ between builds) */
struct Region {
  bool whole = true;
  int64_t left = 0, top = 0, front = 0, right = 0, bottom = 0, back = 0;
};

template <typename Rect>
Region
RegionOfRect(const Rect *rect) {
  Region r;
  if (rect) {
    r.whole = false;
    r.left = rect->left, r.top = rect->top, r.right = rect->right, r.bottom = rect->bottom;
  }
  return r;
}

template <typename Box>
Region
RegionOfBox(const Box *box) {
  Region r;
  if (box) {
    r.whole = false;
    r.left = box->Left, r.top = box->Top, r.front = box->Front;
    r.right = box->Right, r.bottom = box->Bottom, r.back = box->Back;
  }
  return r;
}

/* a lock: "[level <n>, ]rect <l>,<t>-<r>,<b>|box <l>,<t>,<f>-<r>,<b>,<k>|... whole, flags 0x...,
   <resource>"; level < 0: none */
inline std::string
LockArgs(int level, bool box, const Region &region, uint32_t flags, const std::string &resource) {
  std::string s = level >= 0 ? "level " + std::to_string(level) + ", " : std::string();
  s += box ? "box " : "rect ";
  if (region.whole)
    s += "whole";
  else if (box)
    s += std::to_string(region.left) + "," + std::to_string(region.top) + "," + std::to_string(region.front) + "-" +
         std::to_string(region.right) + "," + std::to_string(region.bottom) + "," + std::to_string(region.back);
  else
    s += std::to_string(region.left) + "," + std::to_string(region.top) + "-" + std::to_string(region.right) + "," +
         std::to_string(region.bottom);
  return s + ", flags " + Hex(flags) + ", " + resource;
}

/* Reset, CreateDevice, CreateAdditionalSwapChain: the back buffer they asked for */
template <typename Params>
std::string
PresentArgs(const Params *pp) {
  if (!pp)
    return "no present parameters";
  return "back buffer " + std::to_string(pp->BackBufferWidth) + "x" + std::to_string(pp->BackBufferHeight) +
         ", format " + FormatName(uint32_t(pp->BackBufferFormat)) + ", count " + std::to_string(pp->BackBufferCount) +
         ", windowed " + std::to_string(int(pp->Windowed));
}

/* The whole line; `null_out`: the call succeeded but left its out-pointer null. */
inline std::string
Line(const char *name, int32_t hr, bool null_out, const std::string &args) {
  std::string result = HResultName(hr);
  if (null_out)
    result = (hr == 0 ? std::string("S_OK") : result) + " with a null out-pointer";
  return std::string("[d3d9] ") + name + " failed: " + result + "; " + args;
}

/* Once per (method, result) pair, at most kMaxLines in all. The null-out case counts as its
   own result. */
class Limiter {
public:
  bool
  take(unsigned method, int32_t hr, bool null_out) {
    const uint64_t key = (uint64_t(method) << 33) | (uint64_t(null_out) << 32) | uint32_t(hr);
    std::lock_guard<std::mutex> lock(mutex_);
    for (unsigned i = 0; i < used_; i++)
      if (keys_[i] == key)
        return false;
    if (used_ == kMaxLines)
      return false;
    keys_[used_++] = key;
    return true;
  }

  unsigned
  used() const {
    return used_;
  }

private:
  std::mutex mutex_;
  uint64_t keys_[kMaxLines] = {};
  unsigned used_ = 0;
};

/* The decision and the line, or an empty string when nothing is logged. */
template <typename Args>
std::string
Consider(Limiter &limiter, unsigned method, const char *name, int32_t hr, bool null_out, Args &&args) {
  if (!null_out && Expected(name, hr))
    return std::string();
  if (!limiter.take(method, hr, null_out))
    return std::string();
  return Line(name, hr, null_out, args());
}

} // namespace dxmt::d9fail
