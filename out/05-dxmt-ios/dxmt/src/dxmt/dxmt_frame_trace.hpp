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
#include <map>
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

/* Spec 05 clarification 44: an attachment's texture type: "3d", "2d-array", "cube", "2d" (and
   "2d-ms", "2d-ms-array", "1d", "1d-array" for the others) */
inline const char *
TraceTextureType(WMTTextureType type) {
  switch (type) {
  case WMTTextureType3D: return "3d";
  case WMTTextureType2DArray: return "2d-array";
  case WMTTextureTypeCube:
  case WMTTextureTypeCubeArray: return "cube";
  case WMTTextureType2D: return "2d";
  case WMTTextureType2DMultisample: return "2d-ms";
  case WMTTextureType2DMultisampleArray: return "2d-ms-array";
  case WMTTextureType1D: return "1d";
  case WMTTextureType1DArray: return "1d-array";
  default: return "?";
  }
}

/* "c0 RGBA16Float 1280x720 2d mip 0 slice 0 load" (clarification 44: with the texture's type) */
inline std::string
TraceAttachment(const std::string &name, WMTPixelFormat format, uint64_t width, uint64_t height, unsigned level,
                unsigned slice, WMTLoadAction load, const char *type = nullptr, int texture_number = -1,
                const char *store = nullptr) {
  // clarification 64: "c0 t7 RGB10A2Unorm 1920x1080 2d mip 0 slice 0 load store"
  return name + (texture_number >= 0 ? " t" + std::to_string(texture_number) : std::string()) + " " +
         TraceTexture(format, width, height) + (type ? std::string(" ") + type : std::string()) + " mip " +
         std::to_string(level) + " slice " + std::to_string(slice) + " " + TraceLoadAction(load) +
         (store ? std::string(" ") + store : std::string());
}

/* Spec 05 clarification 64: an attachment's store action */
inline const char *
TraceStoreAction(WMTStoreAction action) {
  switch (action) {
  case WMTStoreActionStore: return "store";
  case WMTStoreActionDontCare: return "dontcare";
  case WMTStoreActionMultisampleResolve: return "resolve";
  case WMTStoreActionStoreAndMultisampleResolve: return "store+resolve";
  default: return "?";
  }
}

/* "depth", "stencil", "depth+stencil" or "-" (bit 1 depth, bit 2 stencil) */
inline const char *
TracePlanes(unsigned planes) {
  switch (planes & 3) {
  case 1: return "depth";
  case 2: return "stencil";
  case 3: return "depth+stencil";
  default: return "-";
  }
}

/* attachments by bit (0-7 c0-c7, 8 depth, 9 stencil), joined with "+" */
inline std::string
TraceAttachmentNames(unsigned bits) {
  std::string names;
  for (unsigned i = 0; i < 10; i++) {
    if (!(bits & (1u << i)))
      continue;
    if (!names.empty())
      names += "+";
    names += i < 8 ? "c" + std::to_string(i) : (i == 8 ? std::string("depth") : std::string("stencil"));
  }
  return names;
}

/* Spec 05 clarification 64: a pass's "cuts" field: what items 69-72 did, or ("would ...") would
   have done with their switch on; "-" for none. */
inline std::string
TraceCutList(uint32_t keep_draws, bool keep, bool keep_would, unsigned ro_store, unsigned ro_store_would,
             unsigned discard_load, unsigned discard_load_would, unsigned discard_store, unsigned discard_store_would,
             unsigned overwrite_load, unsigned overwrite_load_would) {
  std::vector<std::string> entries;
  if (keep)
    entries.push_back("keep-dsv " + std::to_string(keep_draws));
  if (keep_would)
    entries.push_back("would keep-dsv " + std::to_string(keep_draws));
  if (ro_store)
    entries.push_back(std::string("ro-store ") + TracePlanes(ro_store));
  if (ro_store_would)
    entries.push_back(std::string("would ro-store ") + TracePlanes(ro_store_would));
  if (discard_load)
    entries.push_back("discard-load " + TraceAttachmentNames(discard_load));
  if (discard_load_would)
    entries.push_back("would discard-load " + TraceAttachmentNames(discard_load_would));
  if (discard_store)
    entries.push_back("discard-store " + TraceAttachmentNames(discard_store));
  if (discard_store_would)
    entries.push_back("would discard-store " + TraceAttachmentNames(discard_store_would));
  if (overwrite_load)
    entries.push_back("overwrite-load " + TraceAttachmentNames(overwrite_load));
  if (overwrite_load_would)
    entries.push_back("would overwrite-load " + TraceAttachmentNames(overwrite_load_would));
  if (entries.empty())
    return "-";
  std::string list;
  for (auto &entry : entries)
    list += (list.empty() ? "" : ", ") + entry;
  return list;
}

/* Spec 05 clarification 64: what a traced frame's passes moved, and what the cuts saved, for the
   closing lines of trace.txt */
struct TraceCutTotals {
  uint64_t keep_passes = 0, keep_bytes = 0;
  uint64_t ro_planes = 0, ro_bytes = 0;
  uint64_t discard = 0, discard_bytes = 0;
  uint64_t overwrite = 0, overwrite_bytes = 0;
};

struct TraceFrameTotals {
  uint64_t loads = 0, load_bytes = 0;
  uint64_t stores = 0, store_bytes = 0;
  uint64_t copies = 0, copy_bytes = 0;
  TraceCutTotals applied, would;
  uint64_t merged_compute = 0, merged_blit = 0;
};

inline std::string
TraceMegabytes(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f", double(bytes) / (1024.0 * 1024.0));
  return text;
}

/* "loads: 65 attachments, 242.0 MB; stores: 74, 332.0 MB; texture copies: 6, 155.0 MB",
   "cuts applied: keep-dsv 0 passes 0.0 MB, ro-store 0 planes 0.0 MB, discard 0 0.0 MB,
   overwrite-load 0 0.0 MB, merged 0 compute 0 blit", "cuts with the switches on: ..." (the
   would-entries; item 68 has none) */
inline std::vector<std::string>
TraceTotalsLines(const TraceFrameTotals &t) {
  auto cuts = [](const TraceCutTotals &c) {
    return "keep-dsv " + std::to_string(c.keep_passes) + " passes " + TraceMegabytes(c.keep_bytes) + " MB, ro-store " +
           std::to_string(c.ro_planes) + " planes " + TraceMegabytes(c.ro_bytes) + " MB, discard " +
           std::to_string(c.discard) + " " + TraceMegabytes(c.discard_bytes) + " MB, overwrite-load " +
           std::to_string(c.overwrite) + " " + TraceMegabytes(c.overwrite_bytes) + " MB";
  };
  return {
      "loads: " + std::to_string(t.loads) + " attachments, " + TraceMegabytes(t.load_bytes) + " MB; stores: " +
          std::to_string(t.stores) + ", " + TraceMegabytes(t.store_bytes) + " MB; texture copies: " +
          std::to_string(t.copies) + ", " + TraceMegabytes(t.copy_bytes) + " MB",
      "cuts applied: " + cuts(t.applied) + ", merged " + std::to_string(t.merged_compute) + " compute " +
          std::to_string(t.merged_blit) + " blit",
      "cuts with the switches on: " + cuts(t.would),
  };
}

/* ---- spec 05 clarification 44: one line per draw ---- */

/* what a traced draw was asked and which shaders ran it */
struct TraceDrawArgs {
  const char *kind = "draw"; // "draw", "draw indexed", "draw instanced", ...
  bool indexed = false;
  bool indirect = false;
  unsigned indirect_k = 0; // 0: filled in by the encoder ("#?" until then)
  uint32_t count = 0;
  uint32_t start = 0;
  int32_t base = 0;
  uint32_t instances = 1;
  uint32_t start_instance = 0;
  uint32_t vs_hash = 0;
  int vs_writes = 0; // bit 1: SV_RenderTargetArrayIndex, bit 2: SV_ViewportArrayIndex; -1: unknown
  bool gs = false;
  uint32_t gs_hash = 0;
  int gs_writes = 0;
  bool gs_folded = false; // a pass-through geometry shader folded into the vertex stage
};

/* "rtindex", "vpindex", "rtindex,vpindex", "-", or "?" */
inline std::string
TraceWritesText(int bits) {
  if (bits < 0)
    return "?";
  if (!bits)
    return "-";
  std::string text;
  if (bits & 1)
    text = "rtindex";
  if (bits & 2)
    text += text.empty() ? "vpindex" : ",vpindex";
  return text;
}

inline std::string
TraceHash(uint32_t hash) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08x", hash);
  return text;
}

/* "draw indexed instanced indices 36 start 0 base 0 instances 64 start instance 0; vs 1a2b3c4d
   writes rtindex; gs -" (pass and draw numbers are added by the encoder) */
inline std::string
TraceDrawLine(const TraceDrawArgs &draw) {
  std::string line = draw.kind;
  if (draw.indirect) {
    line += " indirect #" + (draw.indirect_k ? std::to_string(draw.indirect_k) : std::string("?"));
  } else {
    line += std::string(draw.indexed ? " indices " : " vertices ") + std::to_string(draw.count) + " start " +
            std::to_string(draw.start) + " base " + (draw.indexed ? std::to_string(draw.base) : std::string("-")) +
            " instances " + std::to_string(draw.instances) + " start instance " + std::to_string(draw.start_instance);
  }
  line += "; vs " + (draw.vs_hash ? TraceHash(draw.vs_hash) : std::string("-")) + " writes " +
          TraceWritesText(draw.vs_writes);
  if (draw.gs)
    line += "; gs " + TraceHash(draw.gs_hash) + " writes " + TraceWritesText(draw.gs_writes) +
            (draw.gs_folded ? " folded" : " emulated");
  else
    line += "; gs -";
  return line;
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
                const std::vector<std::string> &viewports, const std::vector<std::string> &scissors, bool partial,
                int layers = -1, const std::string &writes = std::string(), const std::string &cuts = std::string()) {
  // clarification 44: "layers <n>" (0 when not layered); -1 leaves it out
  std::string line = "pass " + std::to_string(index) + ": " +
                     (attachments.empty() ? std::string("no attachments") : TraceJoin(attachments, ", ")) + "; " +
                     (layers >= 0 ? "layers " + std::to_string(layers) + "; " : std::string()) +
                     std::to_string(draws) + (draws == 1 ? " draw" : " draws");
  // clarification 64: the planes the draws can write, then the cuts (left out when not given)
  if (!writes.empty())
    line += "; writes " + writes;
  if (!cuts.empty())
    line += "; cuts " + cuts;
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
  /* spec 05 clarification 50: a vertex-shader buffer SRV t<slot> read before the frame's draw
     number `serial` */
  bool srv = false;
  unsigned serial = 0;
  /* spec 05 clarification 58: a vertex-shader constant buffer b<slot>, `bytes` bytes read before
     draw `serial`, with the CPU's hash of the same bytes when the buffer is dynamic */
  bool cb = false;
  unsigned bytes = 0;
  bool cpu_hash_valid = false;
  uint64_t cpu_hash = 0;
};

/* Spec 05 clarification 58: FNV-1a, 64-bit (offset basis 0xcbf29ce484222325, prime
   0x100000001b3) */
inline uint64_t
TraceFnv1a64(const void *data, size_t size) {
  uint64_t hash = 0xcbf29ce484222325ull;
  auto bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

/* Spec 05 clarification 58: "cb vs b2 512 bytes gpu 1f2e... cpu 1f2e... 3f800000 ..." with the
   first 256 bytes as 32-bit words, then " CHANGED" when both hashes exist and differ (the
   draw's "pass N draw k" is put in front when it is placed) */
inline std::string
TraceCbReadbackText(unsigned slot, unsigned bytes, const void *data, bool cpu_hash_valid, uint64_t cpu_hash) {
  char hash[24];
  uint64_t gpu_hash = TraceFnv1a64(data, bytes);
  std::string line = "cb vs b" + std::to_string(slot) + " " + std::to_string(bytes) + " bytes gpu ";
  std::snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)gpu_hash);
  line += hash;
  line += " cpu ";
  if (cpu_hash_valid) {
    std::snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)cpu_hash);
    line += hash;
  } else {
    line += "-";
  }
  auto words = static_cast<const uint32_t *>(data);
  unsigned shown = (bytes < 256 ? bytes : 256) / 4;
  for (unsigned i = 0; i < shown; i++) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), " %08x", words[i]);
    line += hex;
  }
  if (cpu_hash_valid && cpu_hash != gpu_hash)
    line += " CHANGED";
  return line;
}

/* at most 4096 bytes per constant buffer, 300 a frame (counted apart from item 50's) */
constexpr unsigned kFrameTraceCbReadbackMaxBytes = 4096;
constexpr unsigned kFrameTraceMaxCbReadbacks = 300;

inline std::string
TraceCbLeftOut(unsigned count) {
  return "vertex-shader constant-buffer read-backs left out: " + std::to_string(count) + " (300 a frame, 64 MB share)";
}

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

/* Spec 05 clarification 50: "srv vs t3 00000000 3f800000 ..." (the draw's "pass N draw k" is put
   in front when it is placed) */
inline std::string
TraceSrvReadbackText(unsigned slot, const uint32_t *values, unsigned count) {
  std::string line = "srv vs t" + std::to_string(slot);
  for (unsigned i = 0; i < count; i++) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), " %08x", values[i]);
    line += hex;
  }
  return line;
}

/* the read-back's line, any kind (spec 05 clarifications 24, 27 and 50) */
inline std::string
TraceReadbackLine(const FrameTraceReadback &readback) {
  auto values = (const uint32_t *)readback.data;
  if (readback.cb)
    return "dxmt-frame " + std::to_string(readback.present) + ": " +
           TraceCbReadbackText(readback.slot, readback.bytes, readback.data, readback.cpu_hash_valid,
                               readback.cpu_hash);
  if (readback.srv)
    return "dxmt-frame " + std::to_string(readback.present) + ": " +
           TraceSrvReadbackText(readback.slot, values, readback.count);
  if (readback.uav)
    return TraceUAVReadback(readback.present, readback.k, readback.slot, values, readback.count);
  return TraceIndirectArgs(readback.present, readback.k, values, readback.count, readback.indexed);
}

/* spec 05 clarification 50: uints read per vertex-shader buffer SRV, and read-backs per frame */
constexpr unsigned kFrameTraceSrvReadbackValues = 64;
constexpr unsigned kFrameTraceMaxSrvReadbacks = 300;

/* the note at the end of a dumped frame's trace when some were left out */
inline std::string
TraceSrvLeftOut(unsigned count) {
  return "vertex-shader buffer read-backs left out: " + std::to_string(count) + " (300 a frame, 64 MB share)";
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
  std::string text; // "uav #4 u0 …", "indirect #3 args …" or "srv vs t3 …"
  bool srv = false; // clarification 50: placed by its draw's serial
  unsigned serial = 0;
};

inline FrameTraceReadbackText
TraceReadbackText(const FrameTraceReadback &readback) {
  std::string line = TraceReadbackLine(readback);
  std::string prefix = "dxmt-frame " + std::to_string(readback.present) + ": ";
  if (line.compare(0, prefix.size(), prefix) == 0)
    line.erase(0, prefix.size());
  // clarification 58: constant buffers are placed under their draw like item 50's views
  return {readback.uav, readback.k, line, readback.srv || readback.cb, readback.serial};
}

/* Spec 05 clarification 50: where a draw's lines end in the frame's lines, and its name
   ("pass 14 draw 3"), by the draw's serial number in the frame. */
struct FrameTraceDrawAnchor {
  size_t last_line;
  std::string name;
};

/* Spec 05 clarification 48: the k of an indirect draw's line ("... draw instanced indirect #5;
   ..."), or 0 */
inline unsigned
TraceDrawIndirectNumber(const std::string &line) {
  size_t at = line.find(" indirect #");
  if (at == std::string::npos || line.compare(0, 5, "pass ") != 0)
    return 0;
  at += 11;
  unsigned k = 0;
  size_t i = at;
  for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; i++)
    k = k * 10 + unsigned(line[i] - '0');
  if (i == at || i >= line.size() || line[i] != ';')
    return 0;
  return k;
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

/* trace.txt's lines: the frame's lines, each read-back right after the line it belongs to:
   - a UAV's after its "dispatch #k" line;
   - indirect arguments after their indirect draw's line (clarification 48), or else after their
     "indirect #k" line;
   - a vertex-shader buffer SRV's after the last line of its draw (clarification 50), named
     "pass N draw k srv vs t<slot> …".
   Any read-back without its line goes at the end. */
inline std::vector<std::string>
FrameTraceFileLines(const std::vector<std::string> &lines, const std::vector<FrameTraceReadbackText> &readbacks,
                    const std::map<unsigned, FrameTraceDrawAnchor> &anchors = {}) {
  std::vector<std::vector<std::string>> after(lines.size());
  std::vector<bool> placed(readbacks.size(), false);
  // the draw lines' indirect numbers come first, so arguments follow their draw
  for (size_t l = 0; l < lines.size(); l++) {
    unsigned draw = TraceDrawIndirectNumber(lines[l]);
    if (!draw)
      continue;
    for (size_t i = 0; i < readbacks.size(); i++)
      if (!placed[i] && !readbacks[i].uav && !readbacks[i].srv && readbacks[i].k == draw) {
        after[l].push_back(readbacks[i].text);
        placed[i] = true;
      }
  }
  for (size_t l = 0; l < lines.size(); l++) {
    unsigned dispatch = TraceLineNumber(lines[l], "dispatch #");
    unsigned indirect = TraceLineNumber(lines[l], "indirect #");
    if (!dispatch && !indirect)
      continue;
    for (size_t i = 0; i < readbacks.size(); i++) {
      if (placed[i] || readbacks[i].srv)
        continue;
      bool here = readbacks[i].uav ? readbacks[i].k == dispatch : readbacks[i].k == indirect;
      if (here) {
        after[l].push_back(readbacks[i].text);
        placed[i] = true;
      }
    }
  }
  for (size_t i = 0; i < readbacks.size(); i++) {
    if (placed[i] || !readbacks[i].srv)
      continue;
    auto anchor = anchors.find(readbacks[i].serial);
    if (anchor != anchors.end() && anchor->second.last_line < lines.size()) {
      after[anchor->second.last_line].push_back(anchor->second.name + " " + readbacks[i].text);
      placed[i] = true;
    }
  }
  std::vector<std::string> out;
  out.reserve(lines.size() + readbacks.size());
  for (size_t l = 0; l < lines.size(); l++) {
    out.push_back(lines[l]);
    for (auto &text : after[l])
      out.push_back(text);
  }
  for (size_t i = 0; i < readbacks.size(); i++)
    if (!placed[i])
      out.push_back(readbacks[i].srv ? "draw #" + std::to_string(readbacks[i].serial) + " " + readbacks[i].text
                                     : readbacks[i].text);
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
  std::map<unsigned, FrameTraceDrawAnchor> draw_anchors; // clarification 50 (encode thread)
};

void WriteFrameTrace(FrameTraceRecord &record);

} // namespace dxmt
