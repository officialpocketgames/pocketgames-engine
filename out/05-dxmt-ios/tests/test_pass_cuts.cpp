// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 round 17, the encoder's side, pure CPU:
//   68  compute and blit passes merging across passes they don't depend on (dxmt_pass_merge.hpp),
//       on fake encoders with exact read and write sets;
//   70  a plane the pass can't write isn't stored; 71 the discard marks and the pass whose store
//       a discard drops; 72 a one-draw pass that overwrites its target (dxmt_pass_cuts.hpp);
//   64  the pass line with texture numbers, store actions, writes and cuts, the would form, the
//       closing lines and their megabytes, and the numbering by first appearance
//       (dxmt_frame_trace.hpp).

#include "test_common.hpp"

#include "dxmt_frame_trace.hpp"
#include "dxmt_pass_cuts.hpp"
#include "dxmt_pass_merge.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace dxmt;

/* ---- item 68 ---- */

struct FakeEncoder {
  MergeKind kind;
  int argbuf = 0;
  std::set<int> reads, writes;
  std::vector<int> commands;
};

bool
Meets(const std::set<int> &a, const std::set<int> &b) {
  for (int x : a)
    if (b.count(x))
      return true;
  return false;
}

struct FakeOps {
  MergeKind
  kind(FakeEncoder *e) {
    return e->kind;
  }
  bool
  sameArguments(FakeEncoder *a, FakeEncoder *b) {
    return a->argbuf == b->argbuf;
  }
  bool
  dependent(FakeEncoder *candidate, FakeEncoder *between) {
    return Meets(between->writes, candidate->reads) || Meets(between->writes, candidate->writes) ||
           Meets(between->reads, candidate->writes);
  }
  void
  merge(FakeEncoder *a, FakeEncoder *b) {
    a->commands.insert(a->commands.end(), b->commands.begin(), b->commands.end());
    a->reads.insert(b->reads.begin(), b->reads.end());
    a->writes.insert(b->writes.begin(), b->writes.end());
    b->commands.clear();
    b->kind = MergeKind::Null;
  }
};

struct Merged {
  std::vector<FakeEncoder> list;
  MergeCounts counts;
  std::vector<FakeEncoder *> alive() {
    std::vector<FakeEncoder *> a;
    for (auto &e : list)
      if (e.kind != MergeKind::Null)
        a.push_back(&e);
    return a;
  }
};

Merged
Merge(std::vector<FakeEncoder> list, unsigned threshold = 64) {
  Merged m{std::move(list), {}};
  std::vector<FakeEncoder *> pointers;
  for (auto &e : m.list)
    pointers.push_back(&e);
  FakeOps ops;
  m.counts = MergeIndependentPasses(pointers.data(), unsigned(pointers.size()), threshold, ops);
  return m;
}

void
TestMerge() {
  // 100 dispatches alternating with 100 copies, each copy reading only its own dispatch's
  // output: one compute pass and one blit pass, commands in recorded order
  std::vector<FakeEncoder> pairs;
  for (int k = 0; k < 100; k++) {
    pairs.push_back({MergeKind::Compute, 1, {1000}, {k}, {2 * k}});
    pairs.push_back({MergeKind::Blit, 0, {k}, {2000 + k}, {2 * k + 1}});
  }
  auto m = Merge(pairs);
  auto alive = m.alive();
  CHECK_EQ(alive.size(), 2u);
  CHECK_EQ(m.counts.compute, 99u);
  CHECK_EQ(m.counts.blit, 99u);
  if (alive.size() == 2) {
    CHECK(alive[0]->kind == MergeKind::Compute && alive[1]->kind == MergeKind::Blit);
    std::vector<int> evens, odds;
    for (int k = 0; k < 100; k++) {
      evens.push_back(2 * k);
      odds.push_back(2 * k + 1);
    }
    CHECK(alive[0]->commands == evens);
    CHECK(alive[1]->commands == odds);
  }

  // the same with each copy's source written by the next dispatch (one scratch buffer): nothing
  std::vector<FakeEncoder> scratch;
  for (int k = 0; k < 20; k++) {
    scratch.push_back({MergeKind::Compute, 1, {1000}, {7}, {2 * k}});
    scratch.push_back({MergeKind::Blit, 0, {7}, {2000 + k}, {2 * k + 1}});
  }
  auto none = Merge(scratch);
  CHECK_EQ(none.alive().size(), 40u);
  CHECK_EQ(none.counts.compute + none.counts.blit, 0u);

  // a render pass reading a dispatch's output between two compute passes, where the second
  // writes what the render pass reads, stops the merge across it
  auto render = Merge({{MergeKind::Compute, 1, {}, {1}, {0}},
                       {MergeKind::Other, 0, {1, 2}, {50}, {1}},
                       {MergeKind::Compute, 1, {}, {2}, {2}}});
  CHECK_EQ(render.alive().size(), 3u);
  // ... and doesn't when it touches nothing the second does
  auto passes = Merge({{MergeKind::Compute, 1, {}, {1}, {0}},
                       {MergeKind::Other, 0, {1}, {50}, {1}},
                       {MergeKind::Compute, 1, {}, {2}, {2}}});
  CHECK_EQ(passes.alive().size(), 2u);

  // different argument buffers don't merge; a barrier stops the reach
  auto args = Merge({{MergeKind::Compute, 1, {}, {1}, {0}}, {MergeKind::Compute, 2, {}, {2}, {1}}});
  CHECK_EQ(args.alive().size(), 2u);
  auto barrier = Merge({{MergeKind::Blit, 0, {}, {1}, {0}}, {MergeKind::Barrier, 0, {}, {}, {1}},
                        {MergeKind::Blit, 0, {}, {2}, {2}}});
  CHECK_EQ(barrier.alive().size(), 3u);

  // the reach counts the encoders left between: past the threshold, the candidate stays
  std::vector<FakeEncoder> far;
  far.push_back({MergeKind::Compute, 1, {}, {1}, {0}});
  for (int k = 0; k < 5; k++)
    far.push_back({MergeKind::Other, 0, {100 + k}, {200 + k}, {1 + k}});
  far.push_back({MergeKind::Compute, 1, {}, {2}, {9}});
  CHECK_EQ(Merge(far, 4).alive().size(), 7u);
  CHECK_EQ(Merge(far, 5).alive().size(), 6u);

  // the switch off, or PG_DXMT_SERIALIZE: the context doesn't call the merge (test_d3d11_cuts
  // checks that the setting is off then)
}

/* ---- items 70-72 ---- */

void
TestRules() {
  // 70: load, store, and the plane not written
  CHECK(RoStoreApplies(CutLoad::Load, CutStore::Store, true, false));
  CHECK(!RoStoreApplies(CutLoad::Load, CutStore::Store, true, true));      // one writing draw
  CHECK(!RoStoreApplies(CutLoad::Clear, CutStore::Store, true, false));    // a folded clear
  CHECK(!RoStoreApplies(CutLoad::Load, CutStore::DontCare, true, false));  // already not stored
  CHECK(!RoStoreApplies(CutLoad::Load, CutStore::Store, false, false));    // a pass without flags
  // read-only views: the encoder takes their planes out of what a draw can write
  uint8_t can_write = uint8_t(3 & ~1u); // depth read-only, a draw that would write both
  CHECK(RoStoreApplies(CutLoad::Load, CutStore::Store, true, can_write & 1));
  CHECK(!RoStoreApplies(CutLoad::Load, CutStore::Store, true, can_write & 2));
  // merged passes add up: one testing pass and one writing pass write
  uint8_t merged = 0 | 1;
  CHECK(!RoStoreApplies(CutLoad::Load, CutStore::Store, true, merged & 1));

  // 72: one draw, no depth or stencil, the target overwritten, the load action load
  CHECK(OverwriteLoadApplies(CutLoad::Load, 1, false, true));
  CHECK(!OverwriteLoadApplies(CutLoad::Load, 2, false, true));
  CHECK(!OverwriteLoadApplies(CutLoad::Load, 1, true, true));
  CHECK(!OverwriteLoadApplies(CutLoad::Load, 1, false, false));
  CHECK(!OverwriteLoadApplies(CutLoad::Clear, 1, false, true));

  CHECK_EQ(AttachmentBytes(1920, 1080, 1, 4), 1920ull * 1080 * 4);
  CHECK_EQ(AttachmentBytes(1280, 720, 4, 1), 1280ull * 720 * 4);
  CHECK_EQ(AttachmentBytes(16, 16, 0, 8), 2048ull);

  // 71: the marks, each kind of write removing them, the load taking them
  DiscardMarks marks;
  marks.mark(7, 0, 0);
  marks.mark(7, 0, 1);
  marks.mark(9, 2, 0);
  CHECK(marks.marked(7, 0, 1));
  marks.written(7, 0, 1); // a copy into one subresource
  CHECK(!marks.marked(7, 0, 1) && marks.marked(7, 0, 0));
  marks.writtenAll(7); // a clear, an upload, a pass storing it, a UAV: any write to the texture
  CHECK(!marks.marked(7, 0, 0) && marks.marked(9, 2, 0));
  CHECK(marks.takeForLoad(9, 2, 0));
  CHECK(!marks.takeForLoad(9, 2, 0)); // once
  CHECK_EQ(marks.size(), 0u);
  CHECK(DiscardHasRectangles(&marks, 2));
  CHECK(!DiscardHasRectangles(nullptr, 0));
  CHECK(!DiscardHasRectangles(&marks, 0));
}

struct Enc {
  bool attaches = false, touches = false;
  Enc *next = nullptr;
};

Enc *
Chain(std::vector<Enc> &list) {
  for (size_t i = 0; i + 1 < list.size(); i++)
    list[i].next = &list[i + 1];
  return list.empty() ? nullptr : &list[0];
}

void
TestDiscardStore() {
  auto attaches = [](Enc *e) { return e->attaches; };
  auto touches = [](Enc *e) { return e->touches; };
  // right after the pass
  std::vector<Enc> a = {{true}, {false}};
  a.pop_back();
  CHECK(LastPassToDropStore(Chain(a), attaches, touches) == &a[0]);
  // an unrelated blit between
  std::vector<Enc> b = {{false}, {true}, {false, false}};
  CHECK(LastPassToDropStore(Chain(b), attaches, touches) == &b[1]);
  // a read of the texture between: no change
  std::vector<Enc> c = {{true}, {false, true}};
  CHECK(LastPassToDropStore(Chain(c), attaches, touches) == nullptr);
  // the last of several passes that attach it
  std::vector<Enc> d = {{true}, {false}, {true}, {false}};
  CHECK(LastPassToDropStore(Chain(d), attaches, touches) == &d[2]);
  // none attaches it
  std::vector<Enc> e = {{false}, {false}};
  CHECK(LastPassToDropStore(Chain(e), attaches, touches) == nullptr);
}

/* ---- item 64 ---- */

void
TestTraceLines() {
  CHECK(TraceAttachment("c0", WMTPixelFormatRGB10A2Unorm, 1920, 1080, 0, 0, WMTLoadActionLoad, "2d", 7, "store") ==
        "c0 t7 RGB10A2Unorm 1920x1080 2d mip 0 slice 0 load store");
  CHECK(TraceAttachment("depth", WMTPixelFormatDepth32Float_Stencil8, 1920, 1080, 0, 0, WMTLoadActionClear, "2d", 3,
                        TraceStoreAction(WMTStoreActionDontCare)) ==
        "depth t3 Depth32Float_Stencil8 1920x1080 2d mip 0 slice 0 clear dontcare");
  CHECK(std::string(TraceStoreAction(WMTStoreActionMultisampleResolve)) == "resolve");
  CHECK(std::string(TraceStoreAction(WMTStoreActionStoreAndMultisampleResolve)) == "store+resolve");
  CHECK(std::string(TraceStoreAction(WMTStoreActionStore)) == "store");

  // the pass line with writes and cuts after the draw count
  CHECK(TraceRenderPass(5, {"c0 t1 RGBA8Unorm 1920x1080 2d mip 0 slice 0 load store"}, 1, {}, {}, false, 0, "-",
                        "overwrite-load c0") ==
        "pass 5: c0 t1 RGBA8Unorm 1920x1080 2d mip 0 slice 0 load store; layers 0; 1 draw; writes -; cuts "
        "overwrite-load c0");
  CHECK(TracePlanes(3) == std::string("depth+stencil"));
  CHECK(TracePlanes(0) == std::string("-"));
  // the cuts field, applied and would forms
  CHECK(TraceCutList(21, true, false, 3, 0, 0, 0, 0, 0, 0, 0) == "keep-dsv 21, ro-store depth+stencil");
  CHECK(TraceCutList(78, false, true, 0, 1, 0, (1u << 0) | (1u << 8), 0, 1u << 9, 0, 1u << 2) ==
        "would keep-dsv 78, would ro-store depth, would discard-load c0+depth, would discard-store stencil, "
        "would overwrite-load c2");
  CHECK(TraceCutList(0, false, false, 0, 0, 1u << 1, 0, 1u << 1, 0, 0, 0) == "discard-load c1, discard-store c1");
  CHECK(TraceCutList(0, false, false, 0, 0, 0, 0, 0, 0, 0, 0) == "-");

  // the closing lines and their megabytes
  TraceFrameTotals t;
  t.loads = 65;
  t.load_bytes = 242ull << 20;
  t.stores = 74;
  t.store_bytes = 332ull << 20;
  t.copies = 6;
  t.copy_bytes = 155ull << 20;
  t.applied.keep_passes = 4;
  t.applied.keep_bytes = 103ull << 20;
  t.would.ro_planes = 8;
  t.would.ro_bytes = (59ull << 20) + (1ull << 19);
  t.merged_compute = 91;
  t.merged_blit = 98;
  auto lines = TraceTotalsLines(t);
  CHECK_EQ(lines.size(), 3u);
  CHECK(lines[0] == "loads: 65 attachments, 242.0 MB; stores: 74, 332.0 MB; texture copies: 6, 155.0 MB");
  CHECK(lines[1] == "cuts applied: keep-dsv 4 passes 103.0 MB, ro-store 0 planes 0.0 MB, discard 0 0.0 MB, "
                    "overwrite-load 0 0.0 MB, merged 91 compute 98 blit");
  CHECK(lines[2] == "cuts with the switches on: keep-dsv 0 passes 0.0 MB, ro-store 8 planes 59.5 MB, discard 0 "
                    "0.0 MB, overwrite-load 0 0.0 MB");

  // texture numbers by first appearance; a new frame starts again
  TraceTextureNumbering numbers;
  CHECK_EQ(numbers.number(0x500), 0u);
  CHECK_EQ(numbers.number(0x300), 1u);
  CHECK_EQ(numbers.number(0x500), 0u);
  CHECK_EQ(numbers.number(0x900), 2u);
  numbers.clear();
  CHECK_EQ(numbers.number(0x900), 0u);
}

} // namespace

int
main() {
  TestMerge();
  TestRules();
  TestDiscardStore();
  TestTraceLines();
  return TestSummary("test_pass_cuts");
}
