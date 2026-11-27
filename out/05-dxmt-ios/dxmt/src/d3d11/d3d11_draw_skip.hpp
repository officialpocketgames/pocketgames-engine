// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 92: a draw that is counted but never reaches an encoder, and why. A program whose
// shaders failed to compile drew with no vertex shader bound, and building the pipeline read the
// null shader (MTLCompiledGraphicsPipeline's constructor). Such a draw is now skipped
// (FinalizeCurrentRenderPipeline returns DrawCallStatus::Invalid), with one line per process:
//   [d3d11] Draw: no vertex shader bound, draw skipped
// and every skipped draw is counted by reason, for the call statistics' work line.
// Pure, so tests/test_d3d11_call_stats.cpp checks it as it is.

#pragma once

#include <atomic>
#include <cstdint>

namespace dxmt {

enum class DrawSkip : uint8_t {
  NoVertexShader, // nothing bound to the vertex stage
  PipelineFailed, // the pipeline couldn't be made
  InvalidState,   // anything else: no index buffer, an unsupported tessellation, no command memory, …
  Count
};

inline const char *const kDrawSkipNames[] = {"no vertex shader", "pipeline failed", "invalid state"};

constexpr const char *kNoVertexShaderLine = "[d3d11] Draw: no vertex shader bound, draw skipped";

/* The skipped draws of the process (every context), by reason. */
class DrawSkips {
public:
  /* A counted draw skipped for `reason`; true when the no-vertex-shader line is to be written now:
     the process's first such draw. */
  bool
  note(DrawSkip reason) {
    counts_[unsigned(reason)].fetch_add(1, std::memory_order_relaxed);
    return reason == DrawSkip::NoVertexShader && !logged_.exchange(true, std::memory_order_relaxed);
  }

  uint64_t
  count(DrawSkip reason) const {
    return counts_[unsigned(reason)].load(std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> counts_[unsigned(DrawSkip::Count)] = {};
  std::atomic<bool> logged_{false};
};

/* One per process (an inline function's static is one across the DLL's files) */
inline DrawSkips &
ProcessDrawSkips() {
  static DrawSkips skips;
  return skips;
}

} // namespace dxmt
