#pragma once
// Changed for MYIOSDECK (spec 05 D4; multi-slice discards; spec 07 A2: LGPL-2.1-or-later lines from dxmt-d3d9 v0.4-d3d9,
// marked inline, see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "dxmt_buffer.hpp"
#include "dxmt_dynamic_policy.hpp"
#include "dxmt_texture.hpp"
#include <deque>
#include <queue>

namespace dxmt {

class DynamicBuffer {
public:
  void incRef();
  void decRef();

  /* spec 07 (LGPL, from v0.4-d3d9): out_minted_fresh, when given, reports whether a new
     allocation had to be minted (a fresh one is zeroed; a recycled one holds old bytes)
     MYIOSDECK: current_seq_id (the chunk being recorded; 0 = not known) lets a buffer
     discarded faster than the GPU retires its allocations grow them (dxmt_dynamic_policy.hpp). */
  Rc<BufferAllocation> allocate(uint64_t coherent_seq_id, bool *out_minted_fresh = nullptr, uint64_t current_seq_id = 0);
  void updateImmediateName(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation, uint32_t suballocation, bool owned_by_command_list);
  void recycle(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation);
  uint32_t nextSuballocation();

  Rc<BufferAllocation>
  immediateName() {
    return name_;
  }

  uint32_t
  immediateSuballocation() {
    return name_suballocation_;
  }

  void *
  immediateMappedMemory() {
    return name_->mappedMemory(name_suballocation_);
  }

  DynamicBuffer(Buffer *buffer, Flags<BufferAllocationFlag> flags);

  struct QueueEntry {
    Rc<BufferAllocation> allocation;
    uint64_t will_free_at;
  };

  /**
   * readonly
   */
  Buffer *buffer;

private:
  Flags<BufferAllocationFlag> flags_;
  std::atomic<uint32_t> refcount_ = {0u};
  // spec 05 D4: a deque, so the run of finished entries can be walked and trimmed
  std::deque<QueueEntry> fifo;
  dxmt::mutex mutex_;
  Rc<BufferAllocation> name_;
  uint32_t name_suballocation_ = 0;
  uint32_t slices_ = 0;          // MYIOSDECK: slices of new allocations (0: as before)
  uint64_t last_mint_seq_ = ~0ull; // MYIOSDECK: chunk of the last new allocation
  bool owned_by_command_list_ = false;
};

class DynamicLinearTexture {
public:
  void incRef();
  void decRef();

  Rc<TextureAllocation> allocate(uint64_t coherent_seq_id);
  void updateImmediateName(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation, bool owned_by_command_list);
  void recycle(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation);

  Rc<TextureAllocation>
  immediateName() {
    return name_;
  };

  void *
  mappedMemory() {
    return name_->mappedMemory;
  }

  DynamicLinearTexture(Texture *buffer, Flags<TextureAllocationFlag> flags);

  struct QueueEntry {
    Rc<TextureAllocation> allocation;
    uint64_t will_free_at;
  };
  /**
   * readonly
   */
  Texture *texture;

private:
  Flags<TextureAllocationFlag> flags_;
  std::atomic<uint32_t> refcount_ = {0u};
  std::queue<QueueEntry> fifo;
  dxmt::mutex mutex_;
  Rc<TextureAllocation> name_;
  bool owned_by_command_list_ = false;
};

} // namespace dxmt
