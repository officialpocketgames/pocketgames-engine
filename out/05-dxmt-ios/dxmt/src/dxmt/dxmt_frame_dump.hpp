// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarifications 28, 31 and 33-36: a dump of the textures a frame writes, in test
// builds. With PG_DXMT_FRAME_DUMP=1 (and the frame trace on, PG_DXMT_FRAME_TRACE=1), the traced
// frames at presents 1800 and 9000 are dumped, each once per process, into the folder
//   C:\users\Public\pg-dumps\frame-<n>
// One read-back buffer per written subresource (texture, mip, slice or face, plane): every
// write in the frame (render pass store, dispatch UAV, copy, clear, resolve) copies into it
// again, so its file is the subresource's state at the end of the frame:
//   t0007-RGBA16Float-1280x720-mip0-slice0.raw     (-face<n> for cubes, -z0 for 3D slice 0)
// For a subresource written more than once, the state after its first write as `-first.raw`,
// from a share of the budget. Plus `present-<format>-<w>x<h>.raw`, the presented image, outside
// the budget, and `index.txt`: every render pass, dispatch, copy, clear and resolve with every
// texture it wrote, dumped or not (and why not). 16 MB per subresource, 256 MB per dumped frame.
// The files are the program's own images: they stay on the device they were made on and the
// spec writer's PC, never in a repository, CI artifact or test data; no name carries a
// program's name. This header is the plain part, for the host tests.

#pragma once

#include "dxmt_frame_trace.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace dxmt {

/* The environment switch, read once per process (defined in dxmt_context.cpp). */
bool FrameDumpSwitch();

constexpr uint64_t kFrameDumpPresents[] = {1800, 9000}; // spec 05 clarification 36
constexpr uint64_t kFrameDumpMaxTextureBytes = 16ull << 20;
constexpr uint64_t kFrameDumpMaxTotalBytes = 256ull << 20;
/* first-write copies may take at most this much of the 256 MB (QUESTIONS.md) */
constexpr uint64_t kFrameDumpMaxFirstBytes = 64ull << 20;
constexpr const char kFrameDumpRoot[] = "C:\\users\\Public\\pg-dumps";

constexpr bool
IsDumpedPresent(uint64_t present) {
  for (auto dumped : kFrameDumpPresents)
    if (present == dumped)
      return true;
  return false;
}

/* Whether the frame ending with `present` is dumped: both switches on, a dump present that is
   also a traced one (clarification 40: or a requested one, `requested`). */
constexpr bool
FrameDumpActive(bool dump_switch, bool trace_switch, uint64_t present, bool requested = false) {
  return dump_switch && trace_switch && (requested || (IsDumpedPresent(present) && IsTracedPresent(present)));
}

/* Spec 05 clarification 50: the API thread's view of whether the frame ending with `present`
   is dumped (the encoder's own test is FrameDumpActive at the frame's start). */
inline bool
FrameDumped(uint64_t present) {
  if (!present || !FrameDumpSwitch() || !FrameTraceSwitch())
    return false;
  bool requested = FrameDumpRequested(present);
  return FrameDumpActive(true, true, present, requested) && FrameTraced(present);
}

/* Bytes per texel of a colour format that is dumped (float, unorm, snorm), or 0 (integer,
   depth, stencil, compressed or unknown). DXMT's swizzle flags are dropped. */
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

/* One plane of a texture as dumped: its name in files ("", "depth" or "stencil"), its format
   label, bytes per texel and the blit option that selects it. */
struct FrameDumpPlane {
  const char *slot;
  const char *format_label;
  unsigned bytes_per_texel;
  WMTBlitOption option;
};

/* The planes a depth texture is dumped as (clarifications 31 and 34): Depth32Float and
   Depth16Unorm as is; Depth32Float_Stencil8 as its depth (depth-from-depth-stencil) and its
   stencil plane (R8, stencil-from-depth-stencil). Other depth formats: none. */
inline unsigned
FrameDumpDepthPlanes(WMTPixelFormat format, FrameDumpPlane planes[2]) {
  switch (static_cast<WMTPixelFormat>(format & 0xFFFF)) {
  case WMTPixelFormatDepth32Float:
    planes[0] = {"depth", "Depth32Float", 4, WMTBlitOptionNone};
    return 1;
  case WMTPixelFormatDepth16Unorm:
    planes[0] = {"depth", "Depth16Unorm", 2, WMTBlitOptionNone};
    return 1;
  case WMTPixelFormatDepth32Float_Stencil8:
    planes[0] = {"depth", "Depth32Float", 4, WMTBlitOptionDepthFromDepthStencil};
    planes[1] = {"stencil", "Stencil8", 1, WMTBlitOptionStencilFromDepthStencil};
    return 2;
  default:
    return 0;
  }
}

/* Why a written texture isn't dumped (clarification 33's index). */
enum class FrameDumpReason { Dumped, Format, Type, NotStored, Budget };

inline const char *
FrameDumpReasonText(FrameDumpReason reason) {
  switch (reason) {
  case FrameDumpReason::Format: return "format";
  case FrameDumpReason::Type: return "type";
  case FrameDumpReason::NotStored: return "not stored";
  case FrameDumpReason::Budget: return "budget";
  default: return "dumped";
  }
}

/* How a texture type's layers are named (clarification 34): slices of 1D/2D (arrays), faces of
   cubes, slice 0 of a 3D texture; multisampled textures and texture buffers aren't dumped. */
enum class FrameDumpLayers { Slice, Face, Z0, NotDumped };

constexpr FrameDumpLayers
FrameDumpLayersOf(WMTTextureType type) {
  switch (type) {
  case WMTTextureType1D:
  case WMTTextureType1DArray:
  case WMTTextureType2D:
  case WMTTextureType2DArray:
    return FrameDumpLayers::Slice;
  case WMTTextureTypeCube:
  case WMTTextureTypeCubeArray:
    return FrameDumpLayers::Face;
  case WMTTextureType3D:
    return FrameDumpLayers::Z0;
  default:
    return FrameDumpLayers::NotDumped;
  }
}

inline const char *
FrameDumpTypeName(WMTTextureType type) {
  switch (type) {
  case WMTTextureType1D: return "1D";
  case WMTTextureType1DArray: return "1DArray";
  case WMTTextureType2D: return "2D";
  case WMTTextureType2DArray: return "2DArray";
  case WMTTextureType2DMultisample: return "2DMS";
  case WMTTextureTypeCube: return "Cube";
  case WMTTextureTypeCubeArray: return "CubeArray";
  case WMTTextureType3D: return "3D";
  case WMTTextureType2DMultisampleArray: return "2DMSArray";
  case WMTTextureTypeTextureBuffer: return "Buffer";
  default: return "?";
  }
}

/* "t0007" */
inline std::string
FrameDumpTextureId(unsigned id) {
  char text[16];
  std::snprintf(text, sizeof(text), "t%04u", id);
  return text;
}

/* "mip0-slice3", "mip0-face5", "mip0-slice1-face2" (a cube array's layer 8), "mip0-z0" */
inline std::string
FrameDumpLayerName(FrameDumpLayers layers, bool cube_array, unsigned mip, unsigned slice) {
  std::string text = "mip" + std::to_string(mip) + "-";
  switch (layers) {
  case FrameDumpLayers::Face:
    if (cube_array)
      return text + "slice" + std::to_string(slice / 6) + "-face" + std::to_string(slice % 6);
    return text + "face" + std::to_string(slice);
  case FrameDumpLayers::Z0:
    return text + "z0";
  default:
    return text + "slice" + std::to_string(slice);
  }
}

/* clarification 42's fallback: "mip0-z32" for depth slice 32 of a 3D mip */
inline std::string
FrameDumpDepthSliceName(unsigned mip, unsigned z) {
  return "mip" + std::to_string(mip) + "-z" + std::to_string(z);
}

/* "t0007-RGBA16Float-1280x720-mip0-slice0.raw", with "-first" before ".raw" for the state after
   the first write */
inline std::string
FrameDumpSubresourceName(unsigned id, const std::string &format_label, uint64_t width, uint64_t height,
                         const std::string &layer, bool first) {
  return FrameDumpTextureId(id) + "-" + format_label + "-" + std::to_string(width) + "x" + std::to_string(height) +
         "-" + layer + (first ? "-first" : "") + ".raw";
}

/* Spec 05 clarification 42: "t0007-RGBA16Float-64x64x64-mip0-3d.raw", every depth slice of a
   3D texture's mip in order (with "-first" for the state after the first write) */
inline std::string
FrameDumpVolumeName(unsigned id, const std::string &format_label, uint64_t width, uint64_t height, uint64_t depth,
                    unsigned mip, bool first) {
  return FrameDumpTextureId(id) + "-" + format_label + "-" + std::to_string(width) + "x" + std::to_string(height) +
         "x" + std::to_string(depth) + "-mip" + std::to_string(mip) + "-3d" + (first ? "-first" : "") + ".raw";
}

/* How a written 3D mip is dumped (clarification 42): whole when it fits the 16 MB per-file
   limit, else depth slices 0, d/2 and d-1 (distinct ones) as separate files. */
struct FrameDump3DPlan {
  bool whole;
  unsigned count;
  unsigned slices[3];
};

inline FrameDump3DPlan
FrameDumpPlan3D(uint64_t width, uint64_t height, uint64_t depth, unsigned texel) {
  FrameDump3DPlan plan{};
  if (!depth)
    depth = 1;
  if (width * height * depth * texel <= kFrameDumpMaxTextureBytes) {
    plan.whole = true;
    return plan;
  }
  unsigned candidates[3] = {0, unsigned(depth / 2), unsigned(depth - 1)};
  for (unsigned z : candidates) {
    bool seen = false;
    for (unsigned i = 0; i < plan.count; i++)
      seen = seen || plan.slices[i] == z;
    if (!seen)
      plan.slices[plan.count++] = z;
  }
  return plan;
}

/* "present-BGRA8Unorm-1280x720.raw" */
inline std::string
FrameDumpPresentName(WMTPixelFormat format, uint64_t width, uint64_t height) {
  return "present-" + TracePixelFormat(format) + "-" + std::to_string(width) + "x" + std::to_string(height) + ".raw";
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

/* "1280x720", or "64x64x64" for a 3D mip */
inline std::string
FrameDumpSizeText(uint64_t width, uint64_t height, uint64_t depth = 1) {
  return std::to_string(width) + "x" + std::to_string(height) + (depth > 1 ? "x" + std::to_string(depth) : "");
}

/* One index line: "pass 14 c0: t0007 RGBA16Float 2D 1280x720 mip0-slice0 -> <file>" or
   "... not dumped (budget)". `layer` may carry a note (clarification 42's 3D slices). */
inline std::string
FrameDumpIndexLine(const std::string &writer, const std::string &slot, unsigned id, const std::string &format_label,
                   WMTTextureType type, const std::string &size, const std::string &layer, FrameDumpReason reason,
                   const std::string &file) {
  std::string line = writer + (slot.empty() ? "" : " " + slot) + ": " + FrameDumpTextureId(id) + " " + format_label +
                     " " + FrameDumpTypeName(type) + " " + size + " " + layer;
  if (reason == FrameDumpReason::Dumped)
    line += " -> " + file;
  else
    line += std::string(" not dumped (") + FrameDumpReasonText(reason) + ")";
  return line;
}

/* the key's `slice` for a whole 3D mip (clarification 42) */
constexpr unsigned kFrameDumpWholeVolume = 0xFFFFFFFFu;

/* ---- clarification 33: one buffer per subresource ---- */

struct FrameDumpKey {
  uint64_t texture; // the texture allocation's Metal texture
  unsigned mip;
  unsigned slice;
  unsigned plane; // 0 colour or depth, 1 stencil
  bool
  operator<(const FrameDumpKey &other) const {
    return std::tie(texture, mip, slice, plane) < std::tie(other.texture, other.mip, other.slice, other.plane);
  }
};

/* What a write of a subresource does in the dump. */
enum class FrameDumpAction {
  Skip,          // not dumped (budget): no copy
  CopyNew,       // its first write: a new last-state buffer, then the copy into it
  Copy,          // a later write: the copy into its buffer again
  CopyWithFirst, // its second write: its buffer copied to a new "-first" buffer, then the copy
};

/* The dump's accounting: each subresource once, whatever its number of writes; a texture over
   16 MB, or one that would pass the 256 MB total, isn't dumped (and stays refused for the
   frame). A subresource's first-write copy is taken at its second write, from what the
   last-state buffers left, at most kFrameDumpMaxFirstBytes in all. */
class FrameDumpLedger {
public:
  FrameDumpAction
  write(const FrameDumpKey &key, uint64_t bytes) {
    auto found = entries_.find(key);
    if (found == entries_.end()) {
      bool fits = bytes <= kFrameDumpMaxTextureBytes && last_bytes_ + first_bytes_ + bytes <= kFrameDumpMaxTotalBytes;
      entries_[key] = {fits, 1};
      if (!fits) {
        refused_++;
        refused_bytes_ += bytes;
        return FrameDumpAction::Skip;
      }
      last_bytes_ += bytes;
      files_++;
      return FrameDumpAction::CopyNew;
    }
    auto &entry = found->second;
    entry.writes++;
    if (!entry.admitted)
      return FrameDumpAction::Skip;
    if (entry.writes == 2 && first_bytes_ + bytes <= kFrameDumpMaxFirstBytes &&
        last_bytes_ + first_bytes_ + bytes <= kFrameDumpMaxTotalBytes) {
      first_bytes_ += bytes;
      first_files_++;
      return FrameDumpAction::CopyWithFirst;
    }
    return FrameDumpAction::Copy;
  }

  /* Spec 05 clarification 50: `bytes` of the first-write share for something else (a vertex
     shader's buffer read-back); false when the share or the total is used up. */
  bool
  takeShare(uint64_t bytes) {
    if (first_bytes_ + bytes > kFrameDumpMaxFirstBytes || last_bytes_ + first_bytes_ + bytes > kFrameDumpMaxTotalBytes)
      return false;
    first_bytes_ += bytes;
    return true;
  }

  /* Spec 05 clarification 50: whether one more vertex-shader buffer read-back of `bytes` may be
     made this frame: at most 300, each out of the first-write share. Counts it either way. */
  bool
  takeSrvReadback(uint64_t bytes) {
    if (srv_readbacks_ >= kFrameTraceMaxSrvReadbacks || !takeShare(bytes)) {
      srv_skipped_++;
      return false;
    }
    srv_readbacks_++;
    return true;
  }

  unsigned
  srvReadbacks() const {
    return srv_readbacks_;
  }

  unsigned
  srvSkipped() const {
    return srv_skipped_;
  }

  /* "dump: 41 files (6 with a first-write copy), 187.5 MB; 2 subresources left out (budget)" */
  std::string
  summary() const {
    char mb[32];
    std::snprintf(mb, sizeof(mb), "%.1f", double(last_bytes_ + first_bytes_) / double(1 << 20));
    return "dump: " + std::to_string(files_) + " files (" + std::to_string(first_files_) +
           " with a first-write copy), " + mb + " MB; " + std::to_string(refused_) +
           " subresources left out (budget)";
  }

  uint64_t lastBytes() const { return last_bytes_; }
  uint64_t firstBytes() const { return first_bytes_; }
  unsigned files() const { return files_; }
  unsigned firstFiles() const { return first_files_; }
  unsigned refused() const { return refused_; }

private:
  struct Entry {
    bool admitted;
    unsigned writes;
  };
  std::map<FrameDumpKey, Entry> entries_;
  uint64_t last_bytes_ = 0;
  uint64_t first_bytes_ = 0;
  unsigned srv_readbacks_ = 0;
  unsigned srv_skipped_ = 0;
  uint64_t refused_bytes_ = 0;
  unsigned files_ = 0;
  unsigned first_files_ = 0;
  unsigned refused_ = 0;
};

/* One read-back buffer of the dump (placed on `host`), written to `directory\name` by the
   finish thread once the frame's command buffers have completed (WriteFrameDumpFile,
   dxmt_context.cpp), then freed. */
struct FrameDumpFile {
  std::string directory;
  std::string name;
  WMT::Reference<WMT::Buffer> buffer;
  void *host = nullptr;
  uint64_t bytes = 0;
};

/* ---- spec 05 clarification 40: a dump on request ---- */

constexpr uint64_t kFrameDumpRequestInterval = 60; // presents between checks
constexpr unsigned kFrameDumpMaxRequests = 4;      // per process
constexpr const char kFrameDumpRequestFile[] = "C:\\users\\Public\\pg-dumps\\frame-request";

/* The request file, as the policy sees it (the real one in dxmt_context.cpp; fakes in tests). */
class FrameDumpRequestFiles {
public:
  virtual ~FrameDumpRequestFiles() = default;
  /* its first line; false when it is missing or unreadable (no request) */
  virtual bool read(std::string &line) = 0;
  /* false when it can't be deleted */
  virtual bool remove() = 0;
};

/* When a request is taken: only with the dump switch, every 60 presents, at most 4 per process,
   not while an earlier dump is still in progress (the request waits for a later check), and
   never again once the file couldn't be deleted. */
class FrameDumpRequestPolicy {
public:
  /* At the end of present `presented`: the present to dump (the next one), or 0. `log` gets the
     line to log, if any. */
  uint64_t
  check(uint64_t presented, bool dump_switch, bool dump_in_progress, FrameDumpRequestFiles &files, std::string &log) {
    log.clear();
    if (!dump_switch || stopped_ || granted_ >= kFrameDumpMaxRequests)
      return 0;
    if (presented == 0 || presented % kFrameDumpRequestInterval != 0)
      return 0;
    if (dump_in_progress)
      return 0;
    std::string line;
    if (!files.read(line))
      return 0;
    if (!files.remove()) {
      stopped_ = true;
      log = "dxmt: frame dump request file can't be deleted; no more requests are taken";
      return 0;
    }
    granted_++;
    uint64_t present = presented + 1;
    log = "dxmt: frame dump requested (" + line + "); dumping present " + std::to_string(present);
    return present;
  }

  unsigned granted() const { return granted_; }
  bool stopped() const { return stopped_; }

private:
  unsigned granted_ = 0;
  bool stopped_ = false;
};

/* Called by CommandQueue::PresentBoundary on the thread that presents, once present `presented`
   is done (defined in dxmt_context.cpp): checks for a request, and marks the next present
   traced and dumped when there is one. */
void FrameDumpCheckRequest(uint64_t presented);
/* a dump frame started (fixed presents; requested ones count from their request) / was written */
void FrameDumpStarted();
void FrameDumpFinished();

/* A finished dumped frame: its folder, index and buffers. */
struct FrameDumpReady {
  std::string directory;
  std::string index;
  std::vector<FrameDumpFile> files;
};

/* Writes the file, then releases the buffer and its memory. */
void WriteFrameDumpFile(FrameDumpFile &file);
/* Writes `text` as `directory\index.txt`. */
void WriteFrameDumpIndex(const std::string &directory, const std::string &text);
/* Releases what wasn't written (after a GPU error). */
void FreeFrameDump(FrameDumpReady &ready);

} // namespace dxmt
