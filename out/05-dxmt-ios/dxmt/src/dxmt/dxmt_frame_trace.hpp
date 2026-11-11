// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 21: a trace of a frame (clarification 24: present 600, then every 1200th,
// at most 8 per process; 40: and requested dumps), since clarification 39 written whole to
// C:\users\Public\pg-dumps\frame-<present>\trace.txt with one summary line in the log, when the
// environment has
// PG_DXMT_FRAME_TRACE=1 (the app sets it in test builds only; the engine is the same binary for
// both, so there is no build switch). This header is the plain part: the switch's value rule,
// which presents are traced, the 600-line cap, and the line formats. ArgumentEncodingContext
// (dxmt_context.cpp) feeds it from the encoders it submits.

#pragma once

#include "dxmt_uav_format_log.hpp"
#include "log/log_cap.hpp"

#include <cstdint>
#include <cstdio>
#include <mutex>
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

/* Spec 05 clarification 24 (amends 21): present 600, then every 1200th (1800, 3000, ...), at
   most kFrameTraceMaxFrames per process. `present` counts from 1: the frame that ends with the
   process's n-th present. */
constexpr uint64_t kFrameTraceFirstPresent = 600;
constexpr uint64_t kFrameTraceInterval = 1200;
constexpr uint64_t kFrameTraceMaxFrames = 8;

constexpr bool
IsTracedPresent(uint64_t present) {
  return present >= kFrameTraceFirstPresent && (present - kFrameTraceFirstPresent) % kFrameTraceInterval == 0 &&
         (present - kFrameTraceFirstPresent) / kFrameTraceInterval < kFrameTraceMaxFrames;
}

/* Spec 05 clarification 40: whether a dump was requested for `present` (process-wide; defined
   in dxmt_context.cpp). */
bool FrameDumpRequested(uint64_t present);

/* A present whose frame is traced: the schedule's, or one a dump was requested for. */
inline bool
FrameTraced(uint64_t present) {
  return IsTracedPresent(present) || FrameDumpRequested(present);
}

/* spec 05 clarification 24: indirect commands of a frame whose arguments are read back */
constexpr unsigned kFrameTraceMaxReadbacks = 128;
/* a log line's text, so that with the logger's prefix it stays within kLogMessageMaxBytes
   (since clarification 39 only the summary line goes to the log; trace.txt has no limit) */
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

/* "dispatch #4 cs 1a2b3c4d tg 8x8x1 groups 160x90x1; u0 RGBA16Float 1280x720" (k 0: no
   number; hash 0: an internal kernel; indirect: no groups). Spec 05 clarification 27: k numbers
   a frame's Direct3D dispatches, for their "uav #k" read-back lines. */
inline std::string
TraceDispatch(unsigned k, uint32_t hash, uint64_t tg_x, uint64_t tg_y, uint64_t tg_z, bool indirect, uint64_t x,
              uint64_t y, uint64_t z, const std::string &uavs) {
  char hash_text[16];
  std::snprintf(hash_text, sizeof(hash_text), "%08x", hash);
  std::string line = std::string("dispatch ") + (k ? "#" + std::to_string(k) + " " : std::string()) +
                     (hash ? std::string("cs ") + hash_text : std::string("internal")) + " tg " +
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

/* Spec 05 clarification 24: an indirect command's own line, "indirect #3 dispatch, argument
   buffer 4096 bytes, offset 48, written earlier in this command buffer: yes". `written` is
   1 / 0, or -1 when not asked (draws). */
inline std::string
TraceIndirect(unsigned k, const char *what, uint64_t buffer_bytes, uint64_t offset, int written) {
  std::string line = "indirect #" + std::to_string(k) + " " + what + ", argument buffer " +
                     std::to_string(buffer_bytes) + " bytes, offset " + std::to_string(offset);
  if (written >= 0)
    line += std::string(", written earlier in this command buffer: ") + (written ? "yes" : "no");
  return line;
}

/* The argument values read back after the command buffer completed: 3 for a dispatch, 4 for
   DrawInstancedIndirect, 5 for DrawIndexedInstancedIndirect (BaseVertexLocation is signed). */
struct FrameTraceReadback {
  uint64_t present;
  unsigned k;
  unsigned count;
  bool indexed;
  const void *data; // shared memory the blit wrote; valid until the command buffer is retired
  /* spec 05 clarification 27: a UAV read-back after dispatch #k, of slot u<slot> */
  bool uav = false;
  unsigned slot = 0;
};

/* Spec 05 clarification 27: "dxmt-frame 1800: uav #4 u0 00000003 0000001a ..." (the first
   `count` uints of the UAV at its view's offset, 8 hex digits each) */
inline std::string
TraceUAVReadback(uint64_t present, unsigned k, unsigned slot, const uint32_t *values, unsigned count) {
  std::string line =
      "dxmt-frame " + std::to_string(present) + ": uav #" + std::to_string(k) + " u" + std::to_string(slot);
  for (unsigned i = 0; i < count; i++) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), " %08x", values[i]);
    line += hex;
  }
  return line;
}

/* spec 05 clarification 27: uints read back per UAV */
constexpr unsigned kFrameTraceUAVReadbackValues = 32;

inline std::string
TraceIndirectArgs(uint64_t present, unsigned k, const uint32_t *values, unsigned count, bool indexed) {
  std::string line = "dxmt-frame " + std::to_string(present) + ": indirect #" + std::to_string(k) + " args";
  for (unsigned i = 0; i < count; i++) {
    // DrawIndexedInstancedIndirect's 4th value, BaseVertexLocation, is an INT
    if (indexed && i == 3)
      line += " " + std::to_string((int32_t)values[i]);
    else
      line += " " + std::to_string(values[i]);
  }
  return line;
}

/* the read-back's line, either kind (spec 05 clarifications 24 and 27) */
inline std::string
TraceReadbackLine(const FrameTraceReadback &readback) {
  auto values = (const uint32_t *)readback.data;
  if (readback.uav)
    return TraceUAVReadback(readback.present, readback.k, readback.slot, values, readback.count);
  return TraceIndirectArgs(readback.present, readback.k, values, readback.count, readback.indexed);
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
    offset_binds_ = 0;
    passes_ = 0;
    indirects_ = 0;
    dispatches_ = 0;
    render_passes_ = 0;
    dispatch_lines_ = 0;
    draws_ = 0;
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

  /* spec 05 clarification 24: the next indirect command's number in this frame, from 1 */
  unsigned
  nextIndirect() {
    return ++indirects_;
  }

  /* spec 05 clarification 27: the next Direct3D dispatch's number in this frame, from 1 */
  unsigned
  nextDispatch() {
    return ++dispatches_;
  }

  /* A line of the traced frame, appended to `out` (spec 05 clarification 39: every line, for
     the frame's trace.txt; no cap, no prefix). */
  void
  line(const std::string &text, std::vector<std::string> &out) {
    if (!active())
      return;
    lines_++;
    out.push_back(text);
  }

  /* spec 05 clarification 39: what the summary line counts */
  void
  countRenderPass(unsigned draws) {
    render_passes_++;
    draws_ += draws;
  }
  void
  countDispatch() {
    dispatch_lines_++;
  }
  unsigned renderPasses() const { return render_passes_; }
  unsigned dispatchCount() const { return dispatch_lines_; }
  uint64_t draws() const { return draws_; }

  /* The frame's closing line: the constant-buffer offset count. */
  void
  finish(std::vector<std::string> &out) {
    if (!active())
      return;
    out.push_back("constant-buffer binds with a non-zero offset: " + std::to_string(offset_binds_));
    present_ = 0;
  }

private:
  uint64_t present_ = 0;
  unsigned lines_ = 0;
  uint64_t offset_binds_ = 0;
  unsigned passes_ = 0;
  unsigned indirects_ = 0;
  unsigned dispatches_ = 0;
  unsigned render_passes_ = 0;
  unsigned dispatch_lines_ = 0;
  uint64_t draws_ = 0;
};

/* ---- spec 05 clarification 39: the whole trace in a file ---- */

/* a read-back's line as it goes into trace.txt, with what places it there */
struct FrameTraceReadbackText {
  bool uav;
  unsigned k;
  std::string text; // "uav #4 u0 …" or "indirect #3 args …"
};

inline FrameTraceReadbackText
TraceReadbackText(const FrameTraceReadback &readback) {
  std::string line = TraceReadbackLine(readback);
  std::string prefix = "dxmt-frame " + std::to_string(readback.present) + ": ";
  if (line.compare(0, prefix.size(), prefix) == 0)
    line.erase(0, prefix.size());
  return {readback.uav, readback.k, line};
}

/* The number after "dispatch #" or "indirect #" at the start of `line`, or 0. */
inline unsigned
TraceLineNumber(const std::string &line, const char *head) {
  size_t length = std::char_traits<char>::length(head);
  if (line.compare(0, length, head) != 0)
    return 0;
  unsigned k = 0;
  size_t i = length;
  for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; i++)
    k = k * 10 + unsigned(line[i] - '0');
  if (i == length || i >= line.size() || line[i] != ' ')
    return 0;
  return k;
}

/* trace.txt's lines: the frame's lines, each read-back right after the line it belongs to (a
   UAV's after its "dispatch #k" line, indirect arguments after their "indirect #k" line), any
   read-back without its line at the end. */
inline std::vector<std::string>
FrameTraceFileLines(const std::vector<std::string> &lines, const std::vector<FrameTraceReadbackText> &readbacks) {
  std::vector<std::string> out;
  std::vector<bool> placed(readbacks.size(), false);
  out.reserve(lines.size() + readbacks.size());
  for (auto &line : lines) {
    out.push_back(line);
    unsigned dispatch = TraceLineNumber(line, "dispatch #");
    unsigned indirect = TraceLineNumber(line, "indirect #");
    if (!dispatch && !indirect)
      continue;
    for (size_t i = 0; i < readbacks.size(); i++) {
      if (placed[i])
        continue;
      bool here = readbacks[i].uav ? readbacks[i].k == dispatch : readbacks[i].k == indirect;
      if (here) {
        out.push_back(readbacks[i].text);
        placed[i] = true;
      }
    }
  }
  for (size_t i = 0; i < readbacks.size(); i++)
    if (!placed[i])
      out.push_back(readbacks[i].text);
  return out;
}

/* "dxmt-frame 1800: 2422 lines, 160 render passes, 141 dispatches, 3010 draws; trace in
   C:\users\Public\pg-dumps\frame-1800\trace.txt" */
inline std::string
FrameTraceSummary(uint64_t present, uint64_t lines, unsigned render_passes, unsigned dispatches, uint64_t draws,
                  const std::string &directory) {
  return "dxmt-frame " + std::to_string(present) + ": " + std::to_string(lines) + " lines, " +
         std::to_string(render_passes) + " render passes, " + std::to_string(dispatches) + " dispatches, " +
         std::to_string(draws) + " draws; trace in " + directory + "\\trace.txt";
}

/* One traced frame's trace: filled by the encode thread; read-back lines added by the finish
   thread as each of its command buffers completes; written as trace.txt once the frame's last
   one has (WriteFrameTrace, dxmt_context.cpp), which also logs the summary line. */
struct FrameTraceRecord {
  uint64_t present = 0;
  std::string directory;
  std::vector<std::string> lines;
  unsigned render_passes = 0;
  unsigned dispatches = 0;
  uint64_t draws = 0;
  std::mutex mutex; // guards readbacks
  std::vector<FrameTraceReadbackText> readbacks;
};

void WriteFrameTrace(FrameTraceRecord &record);

} // namespace dxmt
