// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 round 17, the encoder's side of the pass cuts, as pure rules for the host test:
//   70  a depth or stencil plane the pass can't write isn't stored;
//   71  discarded views aren't stored or loaded (the marks of discarded subresources);
//   72  a one-draw pass that overwrites its target doesn't load it;
// and the megabytes an attachment's load or store moves (item 64's closing lines, item 63's cut
// line): its level's width x height x samples x bytes per texel.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <tuple>

namespace dxmt {

/* The load and store actions as these rules need them (Metal's are mapped by the encoder). */
enum class CutLoad { Load, Clear, DontCare };
enum class CutStore { Store, DontCare, Resolve, StoreAndResolve };

/* 70: a depth or stencil attachment whose load action is load, whose store action is store, and
   whose plane the pass can't write. `writes_known` is false for a pass whose front end doesn't
   say (it can write both). */
constexpr bool
RoStoreApplies(CutLoad load, CutStore store, bool writes_known, bool can_write_plane) {
  return load == CutLoad::Load && store == CutStore::Store && writes_known && !can_write_plane;
}

/* 72: a colour attachment loaded by a pass of one draw, without a depth or stencil attachment,
   whose draw overwrites it whole. */
constexpr bool
OverwriteLoadApplies(CutLoad load, uint32_t draws, bool has_depth_or_stencil, bool target_overwritten) {
  return load == CutLoad::Load && draws == 1 && !has_depth_or_stencil && target_overwritten;
}

/* 71: DiscardView1 with rectangles does nothing, as before */
constexpr bool
DiscardHasRectangles(const void *rects, unsigned count) {
  return count != 0 && rects != nullptr;
}

/* 71, the store side: walking the command buffer's encoders from `first` (through `->next`),
   the last render pass that attaches the discarded subresource (`attaches`), provided no encoder
   after it reads or writes the texture (`touches`); null otherwise. */
template <typename Encoder, typename Attaches, typename Touches>
Encoder *
LastPassToDropStore(Encoder *first, Attaches &&attaches, Touches &&touches) {
  Encoder *last = nullptr;
  for (Encoder *e = first; e; e = e->next)
    if (attaches(e))
      last = e;
  if (!last)
    return nullptr;
  for (Encoder *e = last->next; e; e = e->next)
    if (touches(e))
      return nullptr;
  return last;
}

/* 64: textures numbered in the order the frame's trace first names them */
class TraceTextureNumbering {
public:
  unsigned
  number(uint64_t texture) {
    auto found = numbers_.find(texture);
    if (found != numbers_.end())
      return found->second;
    unsigned n = unsigned(numbers_.size());
    numbers_.emplace(texture, n);
    return n;
  }
  void
  clear() {
    numbers_.clear();
  }

private:
  std::map<uint64_t, unsigned> numbers_;
};

/* Bytes one load or store of an attachment moves. */
constexpr uint64_t
AttachmentBytes(uint32_t width, uint32_t height, uint32_t samples, uint32_t bytes_per_texel) {
  return uint64_t(width) * height * (samples ? samples : 1) * bytes_per_texel;
}

/* 71: subresources (a texture's id, level and slice) marked by a discard; the next pass that
   loads one gets don't-care, and any write takes the mark away. */
class DiscardMarks {
public:
  using Key = std::tuple<uint64_t, uint32_t, uint32_t>;

  void
  mark(uint64_t texture, uint32_t level, uint32_t slice) {
    marks_.insert({texture, level, slice});
  }

  /* A write (a clear, a copy into it, an upload, a pass storing it, an unordered-access write). */
  void
  written(uint64_t texture, uint32_t level, uint32_t slice) {
    marks_.erase({texture, level, slice});
  }

  /* Every subresource of a texture written (a whole-resource copy or upload, a UAV). */
  void
  writtenAll(uint64_t texture) {
    marks_.erase(marks_.lower_bound({texture, 0, 0}), marks_.lower_bound({texture + 1, 0, 0}));
  }

  bool
  marked(uint64_t texture, uint32_t level, uint32_t slice) const {
    return marks_.count({texture, level, slice}) != 0;
  }

  /* A pass attaches it with load action load: true when the load can be don't-care. The mark is
     taken away either way (the pass writes it). */
  bool
  takeForLoad(uint64_t texture, uint32_t level, uint32_t slice) {
    return marks_.erase({texture, level, slice}) != 0;
  }

  size_t
  size() const {
    return marks_.size();
  }

private:
  std::set<Key> marks_;
};

} // namespace dxmt
