#pragma once
// Changed for MYIOSDECK (spec 05 H1; spec 06 B). Upstream: DXMT a3a6e8f, MIT (see LICENSE).

#include "Metal.hpp"
#include "rc/util_rc_ptr.hpp"
#include "util_placed_memory.hpp"
#include "wsi_platform.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>

namespace dxmt {
class VisibilityResultOffsetBumpState {
public:
  void
  beginEncoder() {
    assert(!within_encoder);
    assert(!current_data_is_dirty);
    assert(!~previous_offset);
    within_encoder = true;
  }

  bool
  tryGetNextWriteOffset(bool has_active_occlusion_queries, uint64_t &offset) {
    assert(within_encoder);
    offset = getNextWriteOffset(has_active_occlusion_queries);
    if (offset == previous_offset) {
      return false;
    }
    previous_offset = offset;
    return true;
  }

  uint64_t
  getNextReadOffset() {
    if (within_encoder) {
      if (current_data_is_dirty) {
        current_data_is_dirty = false;
        return ++next_offset;
      }
    }
    assert(!current_data_is_dirty);
    return next_offset;
  }

  void
  endEncoder() {
    assert(within_encoder);
    within_encoder = false;
    previous_offset = ~0uLL;
    if (current_data_is_dirty) {
      next_offset++;
      current_data_is_dirty = false;
    }
  }

  uint64_t
  reset() {
    assert(!within_encoder && "encoder still active");
    assert(!current_data_is_dirty && "encoder still active");
    auto ret = next_offset;
    next_offset = 0;
    return ret;
  }

private:
  uint64_t
  getNextWriteOffset(bool has_active_occlusion_queries) {
    if (has_active_occlusion_queries) {
      current_data_is_dirty = true;
      return next_offset;
    }
    return ~0uLL;
  };

  bool within_encoder = false;
  bool current_data_is_dirty = false;
  uint64_t previous_offset = ~0uLL;
  uint64_t next_offset = 0;
};

class VisibilityResultQuery {
public:
  void
  incRef() {
    refcount_.fetch_add(1u, std::memory_order_acquire);
  }
  void
  decRef() {
    if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
      delete this;
  }

  void
  begin(uint64_t seqId, unsigned offset) {
    accumulated_value_ = 0;
    seq_id_begin = seqId;
    occlusion_counter_begin = offset;
    seq_id_end = ~0uLL;
    occlusion_counter_end = ~0uLL;
  }

  void
  end(uint64_t seqId, unsigned offset) {
    seq_id_end = seqId;
    occlusion_counter_end = offset;
    if (seq_id_begin == seq_id_end && occlusion_counter_begin == occlusion_counter_end) {
      seq_id_issued = seq_id_end;
    }
  }

  uint64_t queryEndAt() {
    return seq_id_end;
  };

  void
  issue(uint64_t seqId, uint64_t const *readbackBuffer, unsigned numResults) {
    assert(seqId >= seq_id_begin);
    assert(seqId <= seq_id_end);
    uint64_t const *start = seqId == seq_id_begin ? readbackBuffer + occlusion_counter_begin : readbackBuffer;
    uint64_t const *end = seqId == seq_id_end ? readbackBuffer + occlusion_counter_end : readbackBuffer + numResults;
    assert(start <= end);
    while (start != end) {
      accumulated_value_ += *start;
      start++;
    }
    seq_id_issued = seqId;
  }

  /* MYIOSDECK spec 05 H1: the query ended in a submission without visibility
     samples; nothing to accumulate, it is complete as of seqId. */
  void
  markIssued(uint64_t seqId) {
    seq_id_issued = seqId;
  }

  bool
  getValue(uint64_t *value) {
    if (seq_id_end <= seq_id_issued) {
      *value = accumulated_value_;
      return true;
    }
    return false;
  }

  void
  reset() {
    accumulated_value_ = 0;
    seq_id_begin = ~0uLL;
    occlusion_counter_begin = ~0uLL;
    seq_id_end = ~0uLL;
    occlusion_counter_end = ~0uLL;
    seq_id_issued = 0;
  };

private:
  uint64_t accumulated_value_ = 0;
  uint64_t seq_id_begin = ~0uLL;
  uint64_t occlusion_counter_begin = ~0uLL;
  uint64_t seq_id_end = ~0uLL;
  uint64_t occlusion_counter_end = ~0uLL;
  uint64_t seq_id_issued = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

class VisibilityResultReadback {
public:
  VisibilityResultReadback(
      WMT::Device device, uint64_t seq_id, uint64_t num_results, std::vector<Rc<VisibilityResultQuery>> &queries
  ) :
      seq_id(seq_id),
      num_results(num_results),
      queries(queries) {
        visibility_result_heap_info.options = WMTResourceHazardTrackingModeUntracked;
        visibility_result_heap_info.memory.set(nullptr);
        visibility_result_heap_info.length = num_results * sizeof(uint64_t);
#ifdef __i386__
        // spec 06 B: page-aligned for newBufferWithBytesNoCopy: (16 KB pages on iOS)
        visibility_result_heap_info.length = PlacedMemoryLength(num_results * sizeof(uint64_t));
        visibility_result_heap_info.memory.set(
            wsi::aligned_malloc(visibility_result_heap_info.length, kPlacedMemoryAlignment)
        );
#endif
        visibility_result_heap = device.newBuffer(visibility_result_heap_info);
      }
  /* MYIOSDECK spec 05 H1: an empty readback, with no buffer, for a submission
     without visibility samples that ends `queries`. Destroyed by the finish
     thread in submission order like any readback, it marks them issued. */
  VisibilityResultReadback(uint64_t seq_id, std::vector<Rc<VisibilityResultQuery>> &&queries) :
      seq_id(seq_id),
      num_results(0),
      queries(std::move(queries)),
      empty(true) {
    visibility_result_heap_info = {};
  }

  ~VisibilityResultReadback() {
    if (empty) {
      for (auto query : queries)
        query->markIssued(seq_id);
      return;
    }
    for (auto query : queries) {
      query->issue(seq_id, (uint64_t *)visibility_result_heap_info.memory.get(), num_results);
    }
#ifdef __i386__
    wsi::aligned_free(visibility_result_heap_info.memory.get());
#endif
  }

  VisibilityResultReadback(const VisibilityResultReadback &) = delete;
  VisibilityResultReadback(VisibilityResultReadback &&) = delete;

  uint64_t seq_id;
  uint64_t num_results;
  std::vector<Rc<VisibilityResultQuery>> queries;
  WMTBufferInfo visibility_result_heap_info;
  WMT::Reference<WMT::Buffer> visibility_result_heap;
  bool empty = false;
};

} // namespace dxmt
