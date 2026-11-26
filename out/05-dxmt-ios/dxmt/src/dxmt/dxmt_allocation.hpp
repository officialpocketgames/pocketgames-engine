#pragma once
// Changed for MYIOSDECK (spec 05 clarification 10: an LGPL-2.1-or-later block from steamac's
// DXMT patch 0023, marked inline, see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
// Changed for MYIOSDECK (spec 05 item 90: the memory ledger's record, the command chunks' share).

#include "dxmt_residency.hpp"
#include "rc/util_rc_ptr.hpp"
#include <atomic>
#include <cstdint>

namespace dxmt {

class Allocation {
public:
  virtual ~Allocation(); // MYIOSDECK spec 05 item 90: out of line, removes mem_record

  void incRef();
  void decRef();

  /* MYIOSDECK spec 05 item 90: the memory ledger's record of this allocation's Metal object (0:
     none), set by whoever made the object and removed when the allocation goes */
  uint64_t mem_record = 0;

  /* Item 90: only command chunks the GPU hasn't finished still hold it (every reference left is
     one AllocationRefTracking took). Read by the ledger under its lock, while the allocation
     can't go (its destructor takes the same lock first). */
  bool
  waitingForGpu() const {
    uint32_t gpu = gpu_refs_.load(std::memory_order_acquire);
    return gpu && refcount_.load(std::memory_order_acquire) == gpu;
  }

  bool
  checkRetained(uint64_t seq_id) {
    // FIXME: is a compare-and-swap necessary?
    if (seq_id == last_retained_seq_id)
      return true;
    last_retained_seq_id = seq_id;
    return false;
  }

  /* LGPL-2.1-or-later, from steamac's DXMT patch 0023 (Jakey-Jakey): put the allocation's
     Metal object into the queue's residency set the first time it is used, and take it
     out when the allocation is freed (decRef). Only called by the encoder thread. */
  void
  useResidencySet(ResidencySetTracker *residency_set, WMT::Object object) {
    if (residency_set_.ptr())
      return;
    residency_set->add(object);
    residency_set_ = residency_set;
    residency_object_ = object;
  }

private:
  friend class AllocationRefTracking; // item 90: counts its references in gpu_refs_
  std::atomic<uint32_t> refcount_ = {0u};
  std::atomic<uint32_t> gpu_refs_ = {0u}; // item 90: of refcount_, those command chunks hold
  uint64_t last_retained_seq_id = 0;
  Rc<ResidencySetTracker> residency_set_;
  WMT::Object residency_object_;
  /* end of LGPL block */
};

class AllocationRefTracking {
public:
  AllocationRefTracking();

  bool track(Allocation *allocation);

  void addStorage(void *ptr, size_t length);

  void clear();

private:
  template <size_t Size = 1> struct RefAddChunk {
    RefAddChunk *next_chunk;
    size_t capacity;
    size_t size;
    Allocation *allocations[Size];
  };
  RefAddChunk<29> chunk_placed;
  RefAddChunk<> *chunk_last = nullptr;
};

} // namespace dxmt
