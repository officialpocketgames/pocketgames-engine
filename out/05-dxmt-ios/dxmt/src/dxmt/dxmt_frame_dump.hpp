// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 28: a one-time dump of the textures one frame writes, in test builds.
// With PG_DXMT_FRAME_DUMP=1 (and the frame trace on, PG_DXMT_FRAME_TRACE=1), the traced frame at
// present 1800 writes each colour attachment after its render pass and each 2D texture UAV
// after its dispatch, as raw texels, to
//   C:\users\Public\pg-dumps\frame-<n>\<seq>-<pass or dispatch hash>-<slot>-<format>-<w>x<h>.raw
// with an index.txt. Only 2D float, unorm and snorm formats; none over 16 MB; 256 MB in total.
// The files are the program's own images: they stay on the device they were made on and the
// spec writer's PC, never in a repository, CI artifact or test data; no name here carries a
// program's name. This header is the plain part, for the host tests: the switch, the frame,
// texel sizes, names and limits.

#pragma once

#include "dxmt_frame_trace.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace dxmt {

/* The environment switch, read once per process (defined in dxmt_context.cpp). */
bool FrameDumpSwitch();

constexpr uint64_t kFrameDumpPresent = 1800;
constexpr uint64_t kFrameDumpMaxTextureBytes = 16ull << 20;
constexpr uint64_t kFrameDumpMaxTotalBytes = 256ull << 20;
constexpr const char kFrameDumpRoot[] = "C:\\users\\Public\\pg-dumps";

/* Whether the frame ending with `present` is dumped: both switches on, present 1800 only. */
constexpr bool
FrameDumpActive(bool dump_switch, bool trace_switch, uint64_t present) {
  return dump_switch && trace_switch && present == kFrameDumpPresent && IsTracedPresent(present);
}

/* Bytes per texel of a 2D colour format that is dumped (float, unorm, snorm), or 0 (integer,
   depth, stencil, compressed or unknown: not dumped). DXMT's swizzle flags are dropped. */
constexpr unsigned
FrameDumpBytesPerTexel(WMTPixelFormat format) {
  switch (static_cast<WMTPixelFormat>(format & 0xFFFF)) {
  case WMTPixelFormatA8Unorm:
  case WMTPixelFormatR8Unorm:
  case WMTPixelFormatR8Unorm_sRGB:
  case WMTPixelFormatR8Snorm:
    return 1;
  case WMTPixelFormatR16Unorm:
  case WMTPixelFormatR16Snorm:
  case WMTPixelFormatR16Float:
  case WMTPixelFormatRG8Unorm:
  case WMTPixelFormatRG8Unorm_sRGB:
  case WMTPixelFormatRG8Snorm:
  case WMTPixelFormatB5G6R5Unorm:
  case WMTPixelFormatA1BGR5Unorm:
  case WMTPixelFormatABGR4Unorm:
  case WMTPixelFormatBGR5A1Unorm:
    return 2;
  case WMTPixelFormatR32Float:
  case WMTPixelFormatRG16Unorm:
  case WMTPixelFormatRG16Snorm:
  case WMTPixelFormatRG16Float:
  case WMTPixelFormatRGBA8Unorm:
  case WMTPixelFormatRGBA8Unorm_sRGB:
  case WMTPixelFormatRGBA8Snorm:
  case WMTPixelFormatBGRA8Unorm:
  case WMTPixelFormatBGRA8Unorm_sRGB:
  case WMTPixelFormatRGB10A2Unorm:
  case WMTPixelFormatBGR10A2Unorm:
  case WMTPixelFormatRG11B10Float:
  case WMTPixelFormatRGB9E5Float:
    return 4;
  case WMTPixelFormatRG32Float:
  case WMTPixelFormatRGBA16Unorm:
  case WMTPixelFormatRGBA16Snorm:
  case WMTPixelFormatRGBA16Float:
    return 8;
  case WMTPixelFormatRGBA32Float:
    return 16;
  default:
    return 0;
  }
}

/* "0007-pass14-c0-RGBA16Float-1280x720.raw", "0012-cs7041a1e5-u1-R16Float-640x360.raw" */
inline std::string
FrameDumpFileName(unsigned seq, const std::string &tag, const std::string &slot, WMTPixelFormat format,
                  uint64_t width, uint64_t height) {
  char number[16];
  std::snprintf(number, sizeof(number), "%04u", seq);
  return std::string(number) + "-" + tag + "-" + slot + "-" + TracePixelFormat(format) + "-" + std::to_string(width) +
         "x" + std::to_string(height) + ".raw";
}

/* "C:\users\Public\pg-dumps\frame-1800" */
inline std::string
FrameDumpDirectory(uint64_t present) {
  return std::string(kFrameDumpRoot) + "\\frame-" + std::to_string(present);
}

/* "cs7041a1e5" for a dispatch's shader hash */
inline std::string
FrameDumpDispatchTag(uint32_t hash) {
  char text[16];
  std::snprintf(text, sizeof(text), "cs%08x", hash);
  return text;
}

/* One texture copied for the dump: its own shared buffer (placed on `host`), written to
   `directory\name` and listed in index.txt by the finish thread once the command buffer has
   completed (WriteFrameDumpFile, dxmt_context.cpp), then freed. */
struct FrameDumpFile {
  std::string directory;
  std::string name;
  std::string index_line;
  WMT::Reference<WMT::Buffer> buffer;
  void *host = nullptr;
  uint64_t bytes = 0;
};

/* Writes the file and its index line, then releases the buffer and its memory. */
void WriteFrameDumpFile(FrameDumpFile &file);

/* The dump's sizes so far: one texture over 16 MB is skipped; once 256 MB were taken, the rest
   are left out. */
class FrameDumpBudget {
public:
  bool
  admit(uint64_t bytes) {
    if (bytes > kFrameDumpMaxTextureBytes) {
      too_large_++;
      return false;
    }
    if (total_ + bytes > kFrameDumpMaxTotalBytes) {
      over_total_++;
      return false;
    }
    total_ += bytes;
    files_++;
    return true;
  }

  /* "dump: 41 files, 187.5 MB; left out: 2 over 16 MB, 0 over the 256 MB total" */
  std::string
  summary() const {
    char mb[32];
    std::snprintf(mb, sizeof(mb), "%.1f", double(total_) / double(1 << 20));
    return "dump: " + std::to_string(files_) + " files, " + mb + " MB; left out: " + std::to_string(too_large_) +
           " over 16 MB, " + std::to_string(over_total_) + " over the 256 MB total";
  }

  uint64_t total() const { return total_; }
  unsigned files() const { return files_; }
  unsigned tooLarge() const { return too_large_; }
  unsigned overTotal() const { return over_total_; }

private:
  uint64_t total_ = 0;
  unsigned files_ = 0;
  unsigned too_large_ = 0;
  unsigned over_total_ = 0;
};

} // namespace dxmt
