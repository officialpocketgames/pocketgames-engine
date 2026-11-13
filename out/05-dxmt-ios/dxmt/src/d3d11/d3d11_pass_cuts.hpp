// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 round 17, what the Direct3D 11 front end tells the encoder about a pass, as pure
// functions for the host test (tests/test_d3d11_cuts.cpp):
//   70  which planes a draw using a depth-stencil state can write;
//   72  whether a draw overwrites each colour target whole (blend, masks, viewport, scissor,
//       fill mode, the draw's shape, the pixel shader's discard);
//   69  when the depth-stencil view stays attached while the application has it unbound.

#pragma once

#include "d3d11.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxmt {

/* ---- item 70 ---- */

constexpr uint8_t kPassWritesDepth = 1;
constexpr uint8_t kPassWritesStencil = 2;

inline bool
TraceStencilOpWrites(D3D11_STENCIL_OP op) {
  return op != D3D11_STENCIL_OP_KEEP;
}

/* depth: the test on and the write mask all; stencil: on, a write mask that isn't zero, and one
   of the six operations not keep. A read-only view's planes are taken out by the encoder. */
inline uint8_t
DepthStencilWrites(const D3D11_DEPTH_STENCIL_DESC &desc) {
  uint8_t writes = 0;
  if (desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL)
    writes |= kPassWritesDepth;
  if (desc.StencilEnable && desc.StencilWriteMask != 0 &&
      (TraceStencilOpWrites(desc.FrontFace.StencilFailOp) || TraceStencilOpWrites(desc.FrontFace.StencilDepthFailOp) ||
       TraceStencilOpWrites(desc.FrontFace.StencilPassOp) || TraceStencilOpWrites(desc.BackFace.StencilFailOp) ||
       TraceStencilOpWrites(desc.BackFace.StencilDepthFailOp) || TraceStencilOpWrites(desc.BackFace.StencilPassOp)))
    writes |= kPassWritesStencil;
  return writes;
}

/* ---- item 72 ---- */

/* Whether a shader model 4/5 shader has a discard instruction, from its DXBC container's SHDR or
   SHEX chunk (opcode 13, D3D10_SB_OPCODE_DISCARD; custom-data blocks, opcode 53, carry their
   length in their second token). True when the container can't be read, which keeps the load. */
inline bool
DxbcMayDiscard(const uint8_t *data, size_t size) {
  auto u32 = [&](size_t at) {
    uint32_t v;
    std::memcpy(&v, data + at, 4);
    return v;
  };
  if (!data || size < 32 || std::memcmp(data, "DXBC", 4) != 0)
    return true;
  uint32_t chunks = u32(28);
  if (32 + uint64_t(chunks) * 4 > size)
    return true;
  for (uint32_t c = 0; c < chunks; c++) {
    uint32_t offset = u32(32 + c * 4);
    if (uint64_t(offset) + 8 > size)
      return true;
    if (std::memcmp(data + offset, "SHDR", 4) != 0 && std::memcmp(data + offset, "SHEX", 4) != 0)
      continue;
    uint32_t chunk_size = u32(offset + 4);
    size_t begin = offset + 8, end = begin + chunk_size;
    if (end > size || chunk_size < 8)
      return true;
    uint32_t tokens = u32(begin + 4); // the program's length in tokens, version token included
    size_t program_end = begin + uint64_t(tokens) * 4;
    if (program_end > end || tokens < 2)
      return true;
    for (size_t at = begin + 8; at < program_end;) {
      uint32_t token = u32(at);
      uint32_t opcode = token & 0x7ff;
      uint32_t length;
      if (opcode == 53) { // custom data: the next token is the block's length in tokens
        if (at + 8 > program_end)
          return true;
        length = u32(at + 4);
      } else {
        length = (token >> 24) & 0x7f;
      }
      if (opcode == 13)
        return true;
      if (length == 0)
        return true; // malformed: can't step over it
      at += size_t(length) * 4;
    }
    return false;
  }
  return true; // no program chunk
}

/* What decides whether a draw overwrites its colour targets whole. */
struct OverwriteDrawFacts {
  bool instanced = false, indirect = false, geometry_or_tessellation = false;
  bool triangle_list_of_3 = false, triangle_strip_of_4 = false;
  bool alpha_to_coverage = false;
  uint32_t sample_mask = 0xffffffff;
  bool one_viewport_covers = false;
  bool scissor_covers = false; // the scissor test off, or its rectangle covering the target
  bool solid_fill = false;
  bool pixel_shader_may_discard = true;
  /* per colour target: blending off and the write mask all four channels */
  std::array<bool, 8> target_plain{};
};

/* The colour targets (bit i for target i) the draw overwrites whole; 0 when any condition that
   isn't per target fails. */
inline uint8_t
OverwriteTargetMask(const OverwriteDrawFacts &f) {
  if (f.instanced || f.indirect || f.geometry_or_tessellation)
    return 0;
  if (!f.triangle_list_of_3 && !f.triangle_strip_of_4)
    return 0;
  if (f.alpha_to_coverage || f.sample_mask != 0xffffffff)
    return 0;
  if (!f.one_viewport_covers || !f.scissor_covers || !f.solid_fill || f.pixel_shader_may_discard)
    return 0;
  uint8_t mask = 0;
  for (unsigned i = 0; i < 8; i++)
    if (f.target_plain[i])
      mask |= uint8_t(1u << i);
  return mask;
}

/* ---- item 69 ---- */

/* The targets an OMSetRenderTargets* call leaves bound (the render-target views by slot, the
   depth-stencil view), and whether its unordered-access views changed. */
struct KeepDsvTargets {
  std::array<const void *, 8> rtvs{};
  unsigned num_rtvs = 0;
  const void *dsv = nullptr;
  /* the output merger's layout these targets give (spec 05 item 77): a keep starts only when
     the call leaves it as the pass has it */
  uint32_t array_length = 0, sample_count = 0, width = 0, height = 0;

  bool
  sameLayout(const KeepDsvTargets &other) const {
    return array_length == other.array_length && sample_count == other.sample_count && width == other.width &&
           height == other.height;
  }
};

enum class KeepDsvStep {
  None,      // nothing to do with a keep: the call is handled as before
  Start,     // the pass stays open with the view attached (switch on), or would (off)
  Resume,    // the view is bound again with the same targets: the keep ends, the pass goes on
  EndPass,   // the keep ends with the pass (other targets)
};

/* The front end's decision for one OMSetRenderTargets* call while a render pass is open.
   `pass` is what the pass has attached; `keeping` whether the view is kept (or would be);
   `srv_views_dsv` whether a view of the pass's depth-stencil texture is bound as a shader
   resource in any stage at that moment; `uavs_changed` whether the call changed the UAVs. */
inline KeepDsvStep
KeepDsvDecide(
    const KeepDsvTargets &pass, bool keeping, const KeepDsvTargets &call, bool uavs_changed, bool srv_views_dsv
) {
  const bool same_rtvs = pass.num_rtvs == call.num_rtvs && pass.rtvs == call.rtvs;
  if (keeping) {
    if (same_rtvs && !uavs_changed && call.dsv == pass.dsv)
      return KeepDsvStep::Resume;
    if (same_rtvs && !uavs_changed && !call.dsv)
      return KeepDsvStep::None; // still unbound: nothing changes
    return KeepDsvStep::EndPass;
  }
  bool has_rtv = false;
  for (auto *rtv : pass.rtvs)
    has_rtv |= rtv != nullptr;
  // at least one render target, and the layout unchanged (a layered target with a non-array
  // view would otherwise draw with another layer count while the view is kept)
  if (pass.dsv && has_rtv && same_rtvs && !uavs_changed && !call.dsv && !srv_views_dsv && pass.sameLayout(call))
    return KeepDsvStep::Start;
  return KeepDsvStep::None;
}

/* The keep as the front end runs it, over its own order of pass ends and starts:
   - passStarted() when a render pass starts, with what it attaches;
   - passEnding(open) from InvalidateCurrentPass, `open` whether a pass is open (the front end
     calls it while idle too, as a new pass starts);
   - draw() for each draw;
   - setTargets(...) for each OMSetRenderTargets* call, after the context's state has the new
     targets;
   - keptTextureBound() when a view of the kept texture is bound as a shader resource.
   A keep with the switch on holds the open pass. With it off (a would-keep), the old pass ends
   as before and the keep starts with the next pass, lasting until that pass ends; a would-keep
   whose pass never started is dropped. What ends a keep comes back as a Note for that pass. */
class KeepDsvTracker {
public:
  struct Note {
    uint32_t draws;
    bool applied;
    uint32_t boundaries; // pass breaks avoided: 1 for the unbinding, 1 more for a rebinding
  };

  struct TargetsResult {
    bool keep_pass_open = false; // switch on: the call doesn't end the pass
    bool end_pass_first = false; // switch off, a start: end the pass before the keep starts
    bool have_note = false;
    Note note{};
  };

  bool
  active() const {
    return active_;
  }
  bool
  applied() const {
    return active_ && applied_;
  }
  const void *
  view() const {
    return active_ ? view_ : nullptr;
  }
  const KeepDsvTargets &
  pass() const {
    return pass_;
  }

  void
  passStarted(const KeepDsvTargets &targets) {
    pass_ = targets;
    pending_ = false; // a would-keep's pass begins
  }

  bool
  passEnding(bool open, Note &note) {
    if (!active_ || pending_ || !open)
      return false;
    note = end();
    return true;
  }

  void
  draw() {
    if (active_ && !pending_)
      draws_++;
  }

  TargetsResult
  setTargets(
      bool render_pass_open, const KeepDsvTargets &call, bool uavs_changed, bool srv_views_dsv, bool on, bool noting
  ) {
    TargetsResult r;
    if (active_ && pending_) {
      // a would-keep whose pass hasn't started: dropped (nothing was drawn with it)
      active_ = pending_ = false;
      view_ = nullptr;
    }
    if (!render_pass_open || (!active_ && !noting))
      return r;
    KeepDsvTargets p = pass_;
    if (active_)
      p.dsv = view_;
    switch (KeepDsvDecide(p, active_, call, uavs_changed, srv_views_dsv)) {
    case KeepDsvStep::Start:
      active_ = true;
      applied_ = on;
      pending_ = !on;
      view_ = p.dsv;
      draws_ = 0;
      boundaries_ = 1;
      r.keep_pass_open = on;
      r.end_pass_first = !on;
      break;
    case KeepDsvStep::Resume:
      boundaries_++;
      r.keep_pass_open = applied_;
      r.have_note = true;
      r.note = end();
      break;
    case KeepDsvStep::EndPass:
      r.have_note = true;
      r.note = end();
      break;
    default:
      break;
    }
    return r;
  }

  /* a view of the kept texture bound as a shader resource: the keep ends (and with the switch
     on, the pass); false when there was nothing to end */
  bool
  keptTextureBound(Note &note) {
    if (!active_)
      return false;
    if (pending_) {
      active_ = pending_ = false;
      view_ = nullptr;
      return false;
    }
    note = end();
    return true;
  }

private:
  Note
  end() {
    Note note{draws_, applied_, boundaries_};
    active_ = applied_ = pending_ = false;
    view_ = nullptr;
    draws_ = boundaries_ = 0;
    return note;
  }

  KeepDsvTargets pass_;
  bool active_ = false, applied_ = false, pending_ = false;
  const void *view_ = nullptr;
  uint32_t draws_ = 0, boundaries_ = 0;
};

} // namespace dxmt
