// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 round 17's cuts, the parts that are pure CPU:
//   65  repeated state calls (d3d11_state_filter.hpp): each of the four, repeated, changes
//       nothing with the switch on; a changed stencil reference, factor or sample mask still
//       does; the switch off;
//   66  discards dirty only the slots holding the resource (d3d11_discard_bound.hpp), never
//       missing one over random bind, unbind and discard sequences;
//   73  the switches' values, the log line and PG_DXMT_SERIALIZE's override (dxmt_cuts.hpp).
// Item 67 is checked on the real DynamicBuffer by bench_dynamic_discard --check.

#include "test_common.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#endif
#include "d3d11_pass_cuts.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "d3d11_discard_bound.hpp"
#include "d3d11_state_filter.hpp"
#include "dxmt_cuts.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace dxmt;

void
TestStateFilter() {
  int state_a = 0, state_b = 0;
  const float ones[4] = {1, 1, 1, 1};
  const float half[4] = {0.5f, 1, 1, 1};
  const float negative_zero[4] = {-0.0f, 1, 1, 1};
  const float zero[4] = {0.0f, 1, 1, 1};

  // OMSetBlendState: the same state, factors (null = 1,1,1,1) and mask
  CHECK(SameBlendCall(&state_a, ones, 0xffffffff, &state_a, nullptr, 0xffffffff));
  CHECK(SameBlendCall(&state_a, ones, 0xffffffff, &state_a, ones, 0xffffffff));
  CHECK(SameBlendCall(nullptr, half, 0xf, nullptr, half, 0xf));
  CHECK(!SameBlendCall(&state_a, ones, 0xffffffff, &state_b, nullptr, 0xffffffff)); // another state
  CHECK(!SameBlendCall(&state_a, ones, 0xffffffff, &state_a, half, 0xffffffff));    // a factor
  CHECK(!SameBlendCall(&state_a, half, 0xffffffff, &state_a, nullptr, 0xffffffff)); // null is 1,1,1,1
  CHECK(!SameBlendCall(&state_a, ones, 0xffffffff, &state_a, ones, 0x7fffffff));    // the sample mask
  CHECK(!SameBlendCall(&state_a, zero, 0xffffffff, &state_a, negative_zero, 0xffffffff)); // any bit

  // OMSetDepthStencilState: the same state (or none both times) and stencil reference
  CHECK(SameDepthStencilCall(&state_a, 3, &state_a, 3));
  CHECK(SameDepthStencilCall(nullptr, 0, nullptr, 0));
  CHECK(!SameDepthStencilCall(&state_a, 3, &state_a, 4)); // a changed reference
  CHECK(!SameDepthStencilCall(&state_a, 3, &state_b, 3));
  CHECK(!SameDepthStencilCall(nullptr, 0, &state_a, 0));

  // IASetIndexBuffer and IASetInputLayout compare the object
  CHECK(SameIndexBuffer(&state_a, &state_a));
  CHECK(SameIndexBuffer(nullptr, nullptr));
  CHECK(!SameIndexBuffer(&state_a, &state_b));

  // the switch: only on and the same value skips the work
  CHECK(StateFilterSkips(true, true));
  CHECK(!StateFilterSkips(true, false));
  CHECK(!StateFilterSkips(false, true));
  CHECK(!StateFilterSkips(false, false));
}

/* ---- item 66 ---- */

/* A binding set of `n` slots holding resource ids (0 = unbound), with dirty bits, iterable
   over its bound slots like DXMT's BindingSet. */
struct FakeSet {
  std::vector<int> slots;
  std::vector<bool> dirty;
  explicit FakeSet(size_t n) : slots(n, 0), dirty(n, false) {}

  std::vector<std::pair<size_t, const int &>>
  bound() const {
    std::vector<std::pair<size_t, const int &>> list;
    for (size_t i = 0; i < slots.size(); i++)
      if (slots[i])
        list.emplace_back(i, slots[i]);
    return list;
  }
  void
  set_dirty(size_t slot) {
    dirty[slot] = true;
  }
  void
  set_dirty() {
    dirty.assign(dirty.size(), true);
  }
  void
  clear_dirty() {
    dirty.assign(dirty.size(), false);
  }
};

/* iterable view for the range-for in DirtyDiscardedSlots */
struct FakeSetView {
  FakeSet &set;
  std::vector<std::pair<size_t, const int &>> list;
  explicit FakeSetView(FakeSet &s) : set(s), list(s.bound()) {}
  auto
  begin() const {
    return list.begin();
  }
  auto
  end() const {
    return list.end();
  }
  void
  set_dirty(size_t slot) {
    set.set_dirty(slot);
  }
  void
  set_dirty() {
    set.set_dirty();
  }
};

void
TestDiscardBound() {
  // a buffer bound in one stage's constant-buffer slot dirties that slot only
  FakeSet vs(14), ps(14), vb(16);
  ps.slots[2] = 7;
  ps.slots[3] = 8;
  vs.slots[0] = 8;
  for (FakeSet *set : {&vs, &ps, &vb}) {
    FakeSetView view(*set);
    DirtyDiscardedSlots(view, true, [](int resource) { return resource == 7; });
  }
  CHECK(ps.dirty[2]);
  CHECK(!ps.dirty[3] && !vs.dirty[0]);
  for (size_t i = 0; i < 14; i++)
    CHECK(i == 2 || !ps.dirty[i]);

  // bound nowhere: nothing
  ps.clear_dirty();
  {
    FakeSetView view(ps);
    DirtyDiscardedSlots(view, true, [](int resource) { return resource == 99; });
  }
  bool any = false;
  for (bool d : ps.dirty)
    any |= d;
  CHECK(!any);

  // bound as a vertex buffer and (through a view) as a shader resource: both
  FakeSet srv(128);
  vb.slots[1] = 5;
  srv.slots[40] = 5;
  for (FakeSet *set : {&vb, &srv}) {
    FakeSetView view(*set);
    DirtyDiscardedSlots(view, true, [](int resource) { return resource == 5; });
  }
  CHECK(vb.dirty[1] && srv.dirty[40]);

  // the switch off: every slot of the kind, as before
  FakeSet off(14);
  off.slots[4] = 3;
  {
    FakeSetView view(off);
    DirtyDiscardedSlots(view, false, [](int) { return false; });
  }
  for (bool d : off.dirty)
    CHECK(d);

  // random binds, unbinds and discards: no slot holding the discarded resource is ever left
  // clean, and only such slots are marked (against the mark-everything rule)
  std::mt19937 rng(63);
  FakeSet sets[3] = {FakeSet(14), FakeSet(14), FakeSet(16)};
  unsigned missed = 0, extra = 0, marked = 0;
  for (int step = 0; step < 20000; step++) {
    FakeSet &set = sets[rng() % 3];
    size_t slot = rng() % set.slots.size();
    switch (rng() % 3) {
    case 0:
      set.slots[slot] = int(rng() % 6) + 1;
      break;
    case 1:
      set.slots[slot] = 0;
      break;
    default: {
      int resource = int(rng() % 6) + 1;
      for (auto &s : sets) {
        s.clear_dirty();
        FakeSetView view(s);
        DirtyDiscardedSlots(view, true, [resource](int bound) { return bound == resource; });
        for (size_t i = 0; i < s.slots.size(); i++) {
          bool holds = s.slots[i] == resource;
          if (holds && !s.dirty[i])
            missed++;
          if (!holds && s.dirty[i])
            extra++;
          marked += s.dirty[i];
        }
      }
    }
    }
  }
  CHECK_EQ(missed, 0u);
  CHECK_EQ(extra, 0u);
  CHECK(marked > 0);
}

/* ---- item 73 ---- */

CutSettings
SettingsFrom(std::map<std::string, std::string> values, unsigned serialize) {
  return CutSettingsFrom(
      [&](const char *variable) -> const char * {
        auto it = values.find(variable);
        return it == values.end() ? nullptr : it->second.c_str();
      },
      serialize
  );
}

void
TestSwitches() {
  auto defaults = SettingsFrom({}, 0);
  CHECK(defaults[CutSwitch::StateFilter] && defaults[CutSwitch::DiscardBound] && defaults[CutSwitch::DeferredRing]);
  CHECK(!defaults[CutSwitch::MergeCompute] && !defaults[CutSwitch::KeepDsv] && !defaults[CutSwitch::RoStore]);
  CHECK(!defaults[CutSwitch::Discard] && !defaults[CutSwitch::OverwriteLoad]);
  CHECK(CutSettingsLine(defaults) == "dxmt: d3d11 cuts: state-filter on, discard-bound on, deferred-ring on, "
                                     "merge-compute off, keep-dsv off, ro-store off, discard off, overwrite-load off");

  // 0 off, 1 on, empty the default, anything else the default with a warning
  auto set = SettingsFrom(
      {{"DXMT_D11_STATE_FILTER", "0"}, {"DXMT_PASS_KEEP_DSV", "1"}, {"DXMT_PASS_RO_STORE", ""},
       {"DXMT_D11_DISCARD_BOUND", "yes"}, {"DXMT_PASS_DISCARD", "2"}},
      0
  );
  CHECK(!set[CutSwitch::StateFilter] && !set.ignored[unsigned(CutSwitch::StateFilter)]);
  CHECK(set[CutSwitch::KeepDsv]);
  CHECK(!set[CutSwitch::RoStore] && !set.ignored[unsigned(CutSwitch::RoStore)]);
  CHECK(set[CutSwitch::DiscardBound] && set.ignored[unsigned(CutSwitch::DiscardBound)]);
  CHECK(!set[CutSwitch::Discard] && set.ignored[unsigned(CutSwitch::Discard)]);
  CHECK(CutSettingsLine(set) == "dxmt: d3d11 cuts: state-filter off, discard-bound on, deferred-ring on, "
                                "merge-compute off, keep-dsv on, ro-store off, discard off, overwrite-load off");

  // PG_DXMT_SERIALIZE >= 1 turns 68-72 off whatever they say; 65-67 stay
  std::map<std::string, std::string> all_on;
  for (auto &info : kCutSwitches)
    all_on[info.variable] = "1";
  auto serialized = SettingsFrom(all_on, 1);
  CHECK(serialized[CutSwitch::StateFilter] && serialized[CutSwitch::DiscardBound] && serialized[CutSwitch::DeferredRing]);
  for (CutSwitch c : {CutSwitch::MergeCompute, CutSwitch::KeepDsv, CutSwitch::RoStore, CutSwitch::Discard,
                      CutSwitch::OverwriteLoad})
    CHECK(!serialized[c] && serialized.serialized[unsigned(c)]);
  CHECK(CutSettingsLine(serialized) ==
        "dxmt: d3d11 cuts: state-filter on, discard-bound on, deferred-ring on, merge-compute off (serialize), "
        "keep-dsv off (serialize), ro-store off (serialize), discard off (serialize), overwrite-load off (serialize)");
  CHECK(SettingsFrom(all_on, 0)[CutSwitch::OverwriteLoad]);
}

/* ---- item 70: what a depth-stencil state lets a draw write ---- */

D3D11_DEPTH_STENCIL_DESC
DepthStencil(bool depth, D3D11_DEPTH_WRITE_MASK mask, bool stencil, UINT8 stencil_mask, D3D11_STENCIL_OP pass_op) {
  D3D11_DEPTH_STENCIL_DESC desc = {};
  desc.DepthEnable = depth;
  desc.DepthWriteMask = mask;
  desc.DepthFunc = D3D11_COMPARISON_LESS;
  desc.StencilEnable = stencil;
  desc.StencilReadMask = 0xff;
  desc.StencilWriteMask = stencil_mask;
  desc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
  desc.BackFace = desc.FrontFace;
  desc.BackFace.StencilPassOp = pass_op;
  return desc;
}

void
TestDepthStencilWrites() {
  using enum_t = uint8_t;
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(true, D3D11_DEPTH_WRITE_MASK_ALL, false, 0, D3D11_STENCIL_OP_KEEP))),
           enum_t(kPassWritesDepth));
  // the test on but no writes; or writes asked with the test off: nothing
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(true, D3D11_DEPTH_WRITE_MASK_ZERO, false, 0, D3D11_STENCIL_OP_KEEP))),
           enum_t(0));
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(false, D3D11_DEPTH_WRITE_MASK_ALL, false, 0, D3D11_STENCIL_OP_KEEP))),
           enum_t(0));
  // stencil: on, a non-zero mask, one operation not keep (here the back face's pass op)
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(false, D3D11_DEPTH_WRITE_MASK_ZERO, true, 0xff, D3D11_STENCIL_OP_REPLACE))),
           enum_t(kPassWritesStencil));
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(false, D3D11_DEPTH_WRITE_MASK_ZERO, true, 0, D3D11_STENCIL_OP_REPLACE))),
           enum_t(0)); // a zero write mask
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(false, D3D11_DEPTH_WRITE_MASK_ZERO, true, 0xff, D3D11_STENCIL_OP_KEEP))),
           enum_t(0)); // every operation keep
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(false, D3D11_DEPTH_WRITE_MASK_ZERO, false, 0xff, D3D11_STENCIL_OP_INCR))),
           enum_t(0)); // stencil off
  CHECK_EQ(enum_t(DepthStencilWrites(DepthStencil(true, D3D11_DEPTH_WRITE_MASK_ALL, true, 0x01, D3D11_STENCIL_OP_INVERT))),
           enum_t(kPassWritesDepth | kPassWritesStencil));
}

/* ---- item 72: a draw that overwrites its target whole ---- */

OverwriteDrawFacts
FullScreenTriangle() {
  OverwriteDrawFacts f;
  f.triangle_list_of_3 = true;
  f.one_viewport_covers = true;
  f.scissor_covers = true;
  f.solid_fill = true;
  f.pixel_shader_may_discard = false;
  f.target_plain[0] = true;
  return f;
}

/* a minimal DXBC container with one program chunk of the given instruction tokens */
std::vector<uint8_t>
Program(const char *fourcc, const std::vector<uint32_t> &instructions) {
  std::vector<uint32_t> program = {0x00050040, uint32_t(2 + instructions.size())};
  program.insert(program.end(), instructions.begin(), instructions.end());
  std::vector<uint8_t> blob(36 + 8 + program.size() * 4, 0);
  std::memcpy(blob.data(), "DXBC", 4);
  uint32_t one_chunk = 1, offset = 36, size = uint32_t(program.size() * 4), total = uint32_t(blob.size());
  std::memcpy(blob.data() + 24, &total, 4);
  std::memcpy(blob.data() + 28, &one_chunk, 4);
  std::memcpy(blob.data() + 32, &offset, 4);
  std::memcpy(blob.data() + 36, fourcc, 4);
  std::memcpy(blob.data() + 40, &size, 4);
  std::memcpy(blob.data() + 44, program.data(), program.size() * 4);
  return blob;
}

void
TestOverwrite() {
  CHECK_EQ(unsigned(OverwriteTargetMask(FullScreenTriangle())), 1u);
  auto strip = FullScreenTriangle();
  strip.triangle_list_of_3 = false;
  strip.triangle_strip_of_4 = true;
  CHECK_EQ(unsigned(OverwriteTargetMask(strip)), 1u);
  // each condition failing in turn keeps the load
  std::vector<OverwriteDrawFacts> failing(11, FullScreenTriangle());
  failing[0].instanced = true;
  failing[1].indirect = true;
  failing[2].geometry_or_tessellation = true;
  failing[3].triangle_list_of_3 = false; // 6 vertices, say
  failing[4].alpha_to_coverage = true;
  failing[5].sample_mask = 0xfffffffe;
  failing[6].one_viewport_covers = false;
  failing[7].scissor_covers = false;
  failing[8].solid_fill = false;
  failing[9].pixel_shader_may_discard = true;
  failing[10].target_plain[0] = false; // blending on or a partial write mask
  for (auto &f : failing)
    CHECK_EQ(unsigned(OverwriteTargetMask(f)), 0u);
  // per target: only the plain ones
  auto two = FullScreenTriangle();
  two.target_plain[2] = true;
  CHECK_EQ(unsigned(OverwriteTargetMask(two)), 5u);

  // the pixel shader's discard, from its program chunk
  auto plain = Program("SHEX", {0x01000036 /* a 1-token instruction, opcode 54 */, 0x0100003e /* ret */});
  CHECK(!DxbcMayDiscard(plain.data(), plain.size()));
  auto discard = Program("SHDR", {0x0300000d /* discard_nz, 3 tokens */, 0x00100012, 0x00000000, 0x0100003e});
  CHECK(DxbcMayDiscard(discard.data(), discard.size()));
  // a custom-data block whose body holds the discard opcode's value isn't an instruction
  auto custom = Program("SHEX", {0x00000035 /* customdata */, 3, 0x0300000d, 0x0100003e});
  CHECK(!DxbcMayDiscard(custom.data(), custom.size()));
  // no program chunk, not a container, cut short, a zero-length instruction: assume a discard
  auto other = Program("ISGN", {0x0100003e});
  CHECK(DxbcMayDiscard(other.data(), other.size()));
  uint8_t junk[40] = {'D', 'X', 'B', 'Y'};
  CHECK(DxbcMayDiscard(junk, sizeof(junk)));
  CHECK(DxbcMayDiscard(plain.data(), 30));
  auto zero = Program("SHEX", {0x00000036});
  CHECK(DxbcMayDiscard(zero.data(), zero.size()));
  CHECK(DxbcMayDiscard(nullptr, 0));
}

/* ---- item 69: the front end's keep, in its own order of pass ends and starts ---- */

/* The context template's calls around KeepDsvTracker (d3d11_context_impl.cpp), step for step:
   - InvalidateCurrentPass: the tracker's passEnding(a pass is open), a note into the open pass,
     the pass ends;
   - a draw with no pass open: SwitchToRenderEncoder calls InvalidateCurrentPass while idle, then
     the pass starts (passStarted with the bound targets); then the tracker's draw();
   - SetRenderTargetsUncounted: the state takes the call's targets; setTargets(...); a note into
     the open pass; the pass is kept open, ended first, or ended as before;
   - binding a view of the kept texture: keptTextureBound, and with the switch on the pass ends.
   A note reaches a pass only while it is open, as the encoder's noteKeepDsv does. */
struct FrontEndModel {
  explicit FrontEndModel(bool on_) : on(on_) {}
  bool on;
  struct Pass {
    unsigned draws = 0, kept = 0, boundaries = 0;
    bool would = false, applied = false;
  };
  std::vector<Pass> passes;
  bool open = false;
  KeepDsvTargets bound;
  KeepDsvTracker keep;
  bool srv_views_dsv = false;
  unsigned would_notes = 0, applied_notes = 0;

  void
  note(const KeepDsvTracker::Note &n) {
    if (!open)
      return;
    auto &p = passes.back();
    p.kept += n.draws;
    p.boundaries += n.boundaries;
    (n.applied ? p.applied : p.would) = true;
    (n.applied ? applied_notes : would_notes)++;
  }
  void
  invalidate() {
    KeepDsvTracker::Note n;
    if (keep.passEnding(open, n))
      note(n);
    open = false;
  }
  void
  draw(unsigned count = 1) {
    if (!open) {
      invalidate(); // SwitchToRenderEncoder's own InvalidateCurrentPass, while idle
      passes.push_back({});
      open = true;
      keep.passStarted(bound);
    }
    passes.back().draws += count;
    for (unsigned i = 0; i < count; i++)
      keep.draw();
  }
  void
  setTargets(const KeepDsvTargets &call, bool uavs_changed = false) {
    bool should_invalidate =
        !(call.rtvs == bound.rtvs && call.num_rtvs == bound.num_rtvs && call.dsv == bound.dsv);
    bound = call;
    auto r = keep.setTargets(open, call, uavs_changed, srv_views_dsv && !keep.active(), on, true);
    if (r.have_note) {
      note(r.note);
      if (r.note.applied && !r.keep_pass_open)
        should_invalidate = true;
    }
    if (r.keep_pass_open)
      should_invalidate = false;
    if (r.end_pass_first && should_invalidate) {
      invalidate();
      should_invalidate = false;
    }
    if (should_invalidate)
      invalidate();
  }
  void
  bindShaderResourceOfKept() {
    KeepDsvTracker::Note n;
    if (keep.keptTextureBound(n)) {
      note(n);
      if (n.applied)
        invalidate();
    }
  }
};

KeepDsvTargets
Targets(const void *rtv, const void *dsv, uint32_t layers = 1) {
  KeepDsvTargets t;
  t.rtvs[0] = rtv;
  t.num_rtvs = 1;
  t.dsv = dsv;
  t.array_length = layers;
  t.sample_count = 1;
  t.width = 1920;
  t.height = 1080;
  return t;
}

void
TestKeepDsv() {
  int rt = 0, other_rt = 0, x = 0;
  // targets with X, a draw, the same without X, a draw, the same with X, a draw (item 77)
  {
    FrontEndModel m(true);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.setTargets(Targets(&rt, nullptr));
    m.draw();
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.invalidate();
    CHECK_EQ(m.passes.size(), 1u);
    CHECK(m.passes.size() == 1 && m.passes[0].draws == 3 && m.passes[0].kept == 1 && m.passes[0].applied);
    CHECK(m.passes.size() == 1 && m.passes[0].boundaries == 2);
    CHECK_EQ(m.applied_notes, 1u);
  }
  {
    // the switch off: three passes, and the would-keep lasts through the pass after the
    // unbinding (the fault item 77 found: it was dropped before that pass started)
    FrontEndModel m(false);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.setTargets(Targets(&rt, nullptr));
    m.draw();
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.invalidate();
    CHECK_EQ(m.passes.size(), 3u);
    CHECK(m.passes.size() == 3 && m.passes[1].would && m.passes[1].kept == 1 && m.passes[1].boundaries == 2);
    CHECK(m.passes.size() == 3 && !m.passes[0].would && !m.passes[2].would);
    CHECK_EQ(m.would_notes, 1u);
  }
  // ... and a would-keep whose pass ends any other way still reaches it, with one boundary
  {
    FrontEndModel m(false);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.setTargets(Targets(&rt, nullptr));
    m.draw(4);
    m.invalidate(); // a clear
    CHECK(m.passes.size() == 2 && m.passes[1].would && m.passes[1].kept == 4 && m.passes[1].boundaries == 1);
  }
  // ... and one whose pass never starts (targets changed again before a draw) is dropped
  {
    FrontEndModel m(false);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.setTargets(Targets(&rt, nullptr));
    m.setTargets(Targets(&other_rt, nullptr));
    m.draw();
    m.invalidate();
    CHECK_EQ(m.would_notes, 0u);
    CHECK(!m.keep.active());
  }
  // targets, the same without the view, the same with it: one pass, the middle draws kept
  {
    FrontEndModel m(true);
    m.setTargets(Targets(&rt, &x));
    m.draw(3);
    m.setTargets(Targets(&rt, nullptr));
    m.draw(4);
    m.setTargets(Targets(&rt, &x));
    m.draw(5);
    CHECK_EQ(m.passes.size(), 1u);
    CHECK(m.passes.size() == 1 && m.passes[0].draws == 12 && m.passes[0].kept == 4);
  }
  // a view of the texture bound as a shader resource at the unbind: no keep
  {
    FrontEndModel m(true);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.srv_views_dsv = true;
    m.setTargets(Targets(&rt, nullptr));
    m.draw();
    CHECK_EQ(m.passes.size(), 2u);
    CHECK_EQ(m.applied_notes, 0u);
  }
  // ... or bound during the keep: the keep and the pass end
  {
    FrontEndModel m(true);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.setTargets(Targets(&rt, nullptr));
    m.draw(2);
    m.bindShaderResourceOfKept();
    m.draw();
    CHECK_EQ(m.passes.size(), 2u);
    CHECK(m.passes.size() == 2 && m.passes[0].kept == 2);
  }
  // other render targets during the keep end the pass; so do changed UAVs at the unbind
  {
    FrontEndModel m(true);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.setTargets(Targets(&rt, nullptr));
    m.draw();
    m.setTargets(Targets(&other_rt, nullptr));
    m.draw();
    CHECK_EQ(m.passes.size(), 2u);
    FrontEndModel u(true);
    u.setTargets(Targets(&rt, &x));
    u.draw();
    u.setTargets(Targets(&rt, nullptr), true);
    u.draw();
    CHECK_EQ(u.passes.size(), 2u);
  }
  // a clear, a copy, a dispatch: anything else that ends the pass ends the keep
  {
    FrontEndModel m(true);
    m.setTargets(Targets(&rt, &x));
    m.draw();
    m.setTargets(Targets(&rt, nullptr));
    m.draw(3);
    m.invalidate(); // the clear
    m.draw();
    CHECK_EQ(m.passes.size(), 2u);
    CHECK(m.passes.size() == 2 && m.passes[0].kept == 3 && m.passes[1].kept == 0);
  }
  // item 77: the call must leave the layout as the pass has it (a layered target with a
  // non-array view): no keep
  {
    FrontEndModel m(true);
    m.setTargets(Targets(&rt, &x, 6));
    m.draw();
    m.setTargets(Targets(&rt, nullptr, 1));
    m.draw();
    CHECK_EQ(m.passes.size(), 2u);
    KeepDsvTargets wide = Targets(&rt, nullptr);
    wide.width = 1280;
    CHECK(KeepDsvDecide(Targets(&rt, &x), false, wide, false, false) == KeepDsvStep::None);
    CHECK(KeepDsvDecide(Targets(&rt, &x), false, Targets(&rt, nullptr), false, false) == KeepDsvStep::Start);
  }
  // the D3D11 RPG's closing passes: one target, the view none, X, none, X, none, X (draws 2, 5,
  // 78, 21, 50, 109), after a pass with other targets
  const unsigned draws[6] = {2, 5, 78, 21, 50, 109};
  for (bool on : {true, false}) {
    FrontEndModel m(on);
    m.setTargets(Targets(&other_rt, &x));
    m.draw(400);
    for (unsigned i = 0; i < 6; i++) {
      m.setTargets(Targets(&rt, i % 2 ? &x : nullptr));
      m.draw(draws[i]);
    }
    m.invalidate();
    if (on) {
      // the first (no view) and one more: X kept through its two unbindings
      CHECK_EQ(m.passes.size(), 1u + 2);
      CHECK(m.passes.size() == 3 && m.passes[2].draws == 5 + 78 + 21 + 50 + 109 && m.passes[2].kept == 78 + 50);
      CHECK_EQ(m.applied_notes, 2u);
    } else {
      CHECK_EQ(m.passes.size(), 1u + 6);
      unsigned would = 0;
      for (auto &p : m.passes)
        would += p.would;
      CHECK_EQ(would, 2u); // the two unbound passes that follow X (item 77)
      CHECK(m.passes.size() == 7 && m.passes[3].would && m.passes[3].kept == 78 && m.passes[5].would &&
            m.passes[5].kept == 50);
    }
  }
}

} // namespace

int
main() {
  TestStateFilter();
  TestDiscardBound();
  TestSwitches();
  TestDepthStencilWrites();
  TestOverwrite();
  TestKeepDsv();
  return TestSummary("test_d3d11_cuts");
}
