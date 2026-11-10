// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 21: a trace of one frame, twice per process (the 600th and the 3600th
// present), in "dxmt-frame <present>: ..." lines, when the environment has
// PG_DXMT_FRAME_TRACE=1 (the app sets it in test builds only; the engine is the same binary for
// both, so there is no build switch). This header is the plain part: the switch's value rule,
// which presents are traced, the 600-line cap, and the line formats. ArgumentEncodingContext
// (dxmt_context.cpp) feeds it from the encoders it submits.

#pragma once

#include "dxmt_uav_format_log.hpp"
#include "log/log_cap.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dxmt {

/* The environment switch, read once per process (defined in dxmt_context.cpp). */
bool FrameTraceSwitch();

/* Only "1" turns the trace on. */
inline bool
FrameTraceSwitchValue(const std::string &value) {
  return value == "1";
}

/* `present` counts from 1: the frame that ends with the process's n-th present. */
constexpr bool
IsTracedPresent(uint64_t present) {
  return present == 600 || present == 3600;
}

constexpr unsigned kFrameTraceMaxLines = 600;
/* each line's text, so that with the logger's prefix it stays within kLogMessageMaxBytes */
constexpr size_t kFrameTraceMaxLineBytes = kLogMessageMaxBytes - 32;

/* ---- formats ---- */

/* A Metal pixel format's name without "MTLPixelFormat", or its number. DXMT's swizzle flags
   (AlphaIsOne, R001, 0R01: bits 21-23) are dropped. */
inline std::string
TracePixelFormat(WMTPixelFormat format) {
  auto base = static_cast<WMTPixelFormat>(format & 0xFFFF);
  if (const char *name = UAVPixelFormatName(base))
    return name;
  switch (base) {
  case WMTPixelFormatDepth16Unorm: return "Depth16Unorm";
  case WMTPixelFormatDepth32Float: return "Depth32Float";
  case WMTPixelFormatStencil8: return "Stencil8";
  case WMTPixelFormatDepth24Unorm_Stencil8: return "Depth24Unorm_Stencil8";
  case WMTPixelFormatDepth32Float_Stencil8: return "Depth32Float_Stencil8";
  case WMTPixelFormatInvalid: return "none";
  default: return "format " + std::to_string(uint32_t(base));
  }
}

inline const char *
TraceLoadAction(WMTLoadAction action) {
  switch (action) {
  case WMTLoadActionDontCare: return "dontcare";
  case WMTLoadActionLoad: return "load";
  case WMTLoadActionClear: return "clear";
  default: return "?";
  }
}

/* "x,y wxh"; a fractional value keeps up to 2 decimals */
inline std::string
TraceNumber(double v) {
  char buffer[32];
  if (v == (double)(int64_t)v)
    std::snprintf(buffer, sizeof(buffer), "%lld", (long long)v);
  else
    std::snprintf(buffer, sizeof(buffer), "%.2f", v);
  return buffer;
}

inline std::string
TraceRect(double x, double y, double w, double h) {
  return TraceNumber(x) + "," + TraceNumber(y) + " " + TraceNumber(w) + "x" + TraceNumber(h);
}

/* Whether a rectangle covers the whole width x height attachment. */
constexpr bool
TraceCovers(double x, double y, double w, double h, uint32_t width, uint32_t height) {
  return x <= 0 && y <= 0 && x + w >= double(width) && y + h >= double(height);
}

/* "RGBA16Float 1280x720" */
inline std::string
TraceTexture(WMTPixelFormat format, uint64_t width, uint64_t height) {
  return TracePixelFormat(format) + " " + std::to_string(width) + "x" + std::to_string(height);
}

/* "c0 RGBA16Float 1280x720 mip 0 slice 0 load" */
inline std::string
TraceAttachment(const std::string &name, WMTPixelFormat format, uint64_t width, uint64_t height, unsigned level,
                unsigned slice, WMTLoadAction load) {
  return name + " " + TraceTexture(format, width, height) + " mip " + std::to_string(level) + " slice " +
         std::to_string(slice) + " " + TraceLoadAction(load);
}

/* Adds `item` to `list` unless it is already there (distinct, in first-use order). */
inline void
TraceAddDistinct(std::vector<std::string> &list, const std::string &item) {
  for (auto &existing : list)
    if (existing == item)
      return;
  list.push_back(item);
}

inline std::string
TraceJoin(const std::vector<std::string> &items, const char *separator) {
  std::string out;
  for (size_t i = 0; i < items.size(); i++) {
    if (i)
      out += separator;
    out += items[i];
  }
  return out;
}

/* "pass 3: c0 RGBA16Float 1280x720 mip 0 slice 0 load, depth ...; 12 draws; viewports 0,0
   1280x720; scissors 0,0 768x620; PARTIAL" */
inline std::string
TraceRenderPass(unsigned index, const std::vector<std::string> &attachments, unsigned draws,
                const std::vector<std::string> &viewports, const std::vector<std::string> &scissors, bool partial) {
  std::string line = "pass " + std::to_string(index) + ": " +
                     (attachments.empty() ? std::string("no attachments") : TraceJoin(attachments, ", ")) + "; " +
                     std::to_string(draws) + (draws == 1 ? " draw" : " draws");
  if (!viewports.empty())
    line += "; viewports " + TraceJoin(viewports, " | ");
  if (!scissors.empty())
    line += "; scissors " + TraceJoin(scissors, " | ");
  if (partial)
    line += "; PARTIAL";
  return line;
}

/* "dispatch cs 1a2b3c4d tg 8x8x1 groups 160x90x1; u0 RGBA16Float 1280x720" (hash 0: an
   internal kernel; groups empty: indirect) */
inline std::string
TraceDispatch(uint32_t hash, uint64_t tg_x, uint64_t tg_y, uint64_t tg_z, bool indirect, uint64_t x, uint64_t y,
              uint64_t z, const std::string &uavs) {
  char hash_text[16];
  std::snprintf(hash_text, sizeof(hash_text), "%08x", hash);
  std::string line = "dispatch " + (hash ? std::string("cs ") + hash_text : std::string("internal")) + " tg " +
                     std::to_string(tg_x) + "x" + std::to_string(tg_y) + "x" + std::to_string(tg_z) + " ";
  line += indirect ? std::string("indirect")
                   : "groups " + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z);
  if (!uavs.empty())
    line += "; " + uavs;
  return line;
}

/* A shader's trace hash: the first 4 bytes of its SHA-1, so "%08x" prints the first 8 hex
   digits of the digest (as the shader dumps and cache names do). */
inline uint32_t
FrameTraceShaderHash(const uint8_t *sha1) {
  return (uint32_t(sha1[0]) << 24) | (uint32_t(sha1[1]) << 16) | (uint32_t(sha1[2]) << 8) | uint32_t(sha1[3]);
}

/* "x,y,z wxhxd" */
inline std::string
TraceBox(uint64_t x, uint64_t y, uint64_t z, uint64_t w, uint64_t h, uint64_t d) {
  return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + " " + std::to_string(w) + "x" +
         std::to_string(h) + "x" + std::to_string(d);
}

/* ---- one process's trace ---- */

class FrameTraceLog {
public:
  /* Called for each submission with the number of the present that ends its frame, or 0 when
     that frame isn't traced. Leaving a traced frame appends its closing lines to `out`. */
  void
  setPresent(uint64_t present, std::vector<std::string> &out) {
    if (present == present_)
      return;
    finish(out);
    present_ = present;
    lines_ = 0;
    dropped_ = 0;
    offset_binds_ = 0;
    passes_ = 0;
  }

  bool
  active() const {
    return present_ != 0;
  }

  uint64_t
  present() const {
    return present_;
  }

  /* the next pass index of this frame */
  unsigned
  nextPass() {
    return passes_++;
  }

  void
  countOffsetBind() {
    if (active())
      offset_binds_++;
  }

  /* A line of the traced frame: prefixed, cut to kFrameTraceMaxLineBytes, appended to `out`
     while fewer than kFrameTraceMaxLines were; counted as not shown after that. */
  void
  line(const std::string &text, std::vector<std::string> &out) {
    if (!active())
      return;
    if (lines_ >= kFrameTraceMaxLines) {
      dropped_++;
      return;
    }
    lines_++;
    out.push_back(CapLogText(prefix() + text, kFrameTraceMaxLineBytes));
  }

  /* The frame's closing lines (outside the cap): the constant-buffer offset count, and how many
     lines weren't shown. */
  void
  finish(std::vector<std::string> &out) {
    if (!active())
      return;
    out.push_back(prefix() + "constant-buffer binds with a non-zero offset: " + std::to_string(offset_binds_));
    if (dropped_)
      out.push_back(prefix() + kLogEllipsis + " " + std::to_string(dropped_) + " more not shown");
    present_ = 0;
  }

private:
  std::string
  prefix() const {
    return "dxmt-frame " + std::to_string(present_) + ": ";
  }

  uint64_t present_ = 0;
  unsigned lines_ = 0;
  uint64_t dropped_ = 0;
  uint64_t offset_binds_ = 0;
  unsigned passes_ = 0;
};

} // namespace dxmt
