// Changed for MYIOSDECK (spec 05 D4; multi-slice discards; spec 07 A2: LGPL-2.1-or-later lines from dxmt-d3d9 v0.4-d3d9,
// marked inline, see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "dxmt_dynamic.hpp"
#include "dxmt_texture.hpp"

namespace dxmt {
DynamicBuffer::DynamicBuffer(Buffer *buffer, Flags<BufferAllocationFlag> flags) :
    buffer(buffer),
    flags_(flags),
    name_(buffer->current()) {}

void
DynamicBuffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
DynamicBuffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<BufferAllocation>
DynamicBuffer::allocate(uint64_t coherent_seq_id, bool *out_minted_fresh, uint64_t current_seq_id) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  Rc<BufferAllocation> ret;
  for (;;) {
    if (fifo.empty()) {
      break;
    }
    auto entry = fifo.front();
    if (entry.will_free_at > coherent_seq_id) {
      break;
    }
    ret = std::move(entry.allocation);
    fifo.pop_front();
    break;
  }
#ifdef DXMT_IOS
  // Spec 05 D4: the queue of retired allocations never shrank. Entries are
  // queued in order of their retire point, so the ones whose GPU work has
  // finished are exactly the leading run; keep at most 64 of them.
  constexpr size_t kMaxFinishedEntries = 64;
  size_t finished = 0;
  for (auto &entry : fifo) {
    if (entry.will_free_at > coherent_seq_id)
      break;
    finished++;
  }
  while (finished > kMaxFinishedEntries) {
    fifo.pop_front();
    finished--;
  }
#endif
  /* spec 07: LGPL-2.1-or-later, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6): report
     whether nothing retired could be handed back and a new allocation was minted */
  if (out_minted_fresh)
    *out_minted_fresh = !ret.ptr();
  /* end of the spec 07 lines */
  if (!ret.ptr()) {
    // MYIOSDECK (dynamic-buffer discard): with the caller's chunk known, a new
    // allocation needed in the same chunk as the previous one doubles the
    // slices of new allocations (dxmt_dynamic_policy.hpp).
    if (current_seq_id != 0) {
      slices_ = NextDynamicSlices(slices_, current_seq_id == last_mint_seq_, buffer->length());
      last_mint_seq_ = current_seq_id;
    }
    ret = buffer->allocate(flags_, slices_);
  }
  return ret;
}

void
DynamicBuffer::updateImmediateName(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation, uint32_t suballocation, bool owned_by_command_list) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (!owned_by_command_list_)
    fifo.push_back(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
  name_ = std::move(allocation);
  name_suballocation_ = suballocation;
  owned_by_command_list_ = owned_by_command_list;
}

void
DynamicBuffer::recycle(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (owned_by_command_list_) {
    if (name_.ptr() == allocation.ptr()) {
      owned_by_command_list_ = false;
      auto _ = std::move(allocation);
      return;
    }
  }
  fifo.push_back(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

uint32_t
DynamicBuffer::nextSuballocation() {
  if (name_->hasSuballocatoin(name_suballocation_ + 1)) {
    return ++name_suballocation_;
  }
  return 0;
}

DynamicLinearTexture::DynamicLinearTexture(Texture *texture, Flags<TextureAllocationFlag> flags) :
    texture(texture),
    flags_(flags),
    name_(texture->current()) {}

void
DynamicLinearTexture::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
DynamicLinearTexture::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<TextureAllocation>
DynamicLinearTexture::allocate(uint64_t coherent_seq_id) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  Rc<TextureAllocation> ret;
  for (;;) {
    if (fifo.empty()) {
      break;
    }
    auto entry = fifo.front();
    if (entry.will_free_at > coherent_seq_id) {
      break;
    }
    ret = std::move(entry.allocation);
    fifo.pop();
    break;
  }
  if (!ret.ptr())
    ret = texture->allocate(flags_);
  return ret;
}

void
DynamicLinearTexture::updateImmediateName(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation, bool owned_by_command_list) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (!owned_by_command_list_)
    fifo.push(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
  name_ = std::move(allocation);
  owned_by_command_list_ = owned_by_command_list;
}

void
DynamicLinearTexture::recycle(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (owned_by_command_list_) {
    if (name_.ptr() == allocation.ptr()) {
      owned_by_command_list_ = false;
      auto _ = std::move(allocation);
      return;
    }
  }
  fifo.push(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

} // namespace dxmt
