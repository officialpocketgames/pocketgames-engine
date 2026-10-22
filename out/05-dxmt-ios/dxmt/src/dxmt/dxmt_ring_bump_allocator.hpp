#pragma once
// Changed for MYIOSDECK (spec 05 D6, D7; spec 06 A, B2, B3; spec 07 A2: LGPL-2.1-or-later blocks
// from dxmt-d3d9 v0.4-d3d9 (e8dd4c6), marked inline, see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT.

#include "Metal.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_bitness.hpp"
#include "util_math.hpp"
#include <cassert>
#include <mutex>
#include <queue>
#ifdef DXMT_IOS
#include "util_ios_switch.hpp"
#include "util_placed_memory.hpp"
#include "wsi_platform.hpp"
#endif

namespace dxmt {

// Spec 06 B3: the i386 build uses 8 MB blocks; the program, its data and every
// block share one 4 GB window.
constexpr size_t kStagingBlockSize = kI386Build ? 0x800000 : 0x2000000; // 8MB / 32MB
constexpr size_t kStagingBlockSizeForDeferredContext = 0x200000; // 2MB
constexpr size_t kStagingBlockLifetime = 300;

#ifdef DXMT_IOS
/* Spec 05 D7: switch DXMT_RING_OVERSIZE_REUSE (read once; unset or empty =
   on in the i386 build only (spec 06 A), "0" = off, anything else = on).
   Keeps at most two blocks larger than the ring's block size, of up to 16 MB,
   for reuse. */
constexpr size_t kOversizeReuseMaxBytes = 16ull << 20;
constexpr unsigned kOversizeReuseMaxBlocks = 2;

inline bool
RingOversizeReuseEnabled() {
  static const bool enabled = env::readSwitch("DXMT_RING_OVERSIZE_REUSE", kI386Build);
  return enabled;
}
#endif

template <typename Allocator, size_t BlockSize = kStagingBlockSize, class mutex = dxmt::mutex> class RingBumpState {

public:

  static constexpr size_t block_size = BlockSize;

  /* ---- spec 07: LGPL-2.1-or-later block, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6)
     dxmt_ring_bump_allocator.hpp: single-writer option, preallocate, seal_latest ---- */
  RingBumpState(Allocator &&allocator, bool single_writer = false) : allocator_(std::move(allocator)) {
#ifdef DXMT_DEBUG
    single_writer_ = single_writer;
#else
    (void)single_writer;
#endif
  }

  void
  preallocate(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      fifo.push({
          .allocated_size = 0,
          .total_size = BlockSize,
          .last_used_seq_id = 0,
          .inc_time_to_live = 0,
          .block = allocator_.allocate(BlockSize),
      });
    }
  }

  void
  seal_latest() {
    std::lock_guard<mutex> lock(mutex_);
    note_single_writer();
    if (fifo.empty())
      return;
    auto &latest = fifo.back();
    latest.allocated_size = latest.total_size;
  }
  /* ---- end of LGPL block ---- */

  std::pair<typename Allocator::Block &, uint64_t>
  allocate(uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment);

  void free_blocks(uint64_t coherent_id);

private:
  struct Allocation {
    size_t allocated_size;
    size_t total_size;
    uint64_t last_used_seq_id;
    uint64_t inc_time_to_live;
    Allocator::Block block;
    /* spec 05 D7: an oversized block kept for reuse like a normal one */
    bool reusable_oversize = false;
  };

  Allocation &allocate_or_reuse_block(uint64_t seq_id, uint64_t coherent_id, size_t block_size);

  /* True if `allocation` is thrown away after use (upstream: any block whose
     size isn't BlockSize). */
  bool
  is_adhoc(const Allocation &allocation) const {
    return allocation.total_size != BlockSize && !allocation.reusable_oversize;
  }

  void
  pop_front() {
#ifdef DXMT_IOS
    if (fifo.front().reusable_oversize)
      kept_oversize_--;
#endif
    fifo.pop();
  }

#ifdef DXMT_IOS
  unsigned kept_oversize_ = 0;
#endif

  /* spec 07: LGPL-2.1-or-later, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6) */
#ifdef DXMT_DEBUG
  void
  note_single_writer() {
    if (!single_writer_)
      return;
    uint32_t tid = this_thread::get_id();
    if (!writer_tid_)
      writer_tid_ = tid;
    assert(writer_tid_ == tid && "RingBumpState single-writer invariant violated");
  }
  bool single_writer_ = false;
  uint32_t writer_tid_ = 0;
#else
  void note_single_writer() {}
#endif

  std::pair<typename Allocator::Block &, uint64_t>
  suballocate(Allocation &allocation, size_t size, size_t alignment) {
    auto offset = align(allocation.allocated_size, alignment);
    allocation.allocated_size = offset + size;
    return {allocation.block, offset};
  };

  std::queue<Allocation> fifo;
  mutex mutex_;
  Allocator allocator_;
};

class GpuPrivateBufferBlockAllocator {
public:
  GpuPrivateBufferBlockAllocator(WMT::Device device, WMTResourceOptions block_options) {
    device_ = device;
    buffer_info_.memory.set(nullptr);
    buffer_info_.options = block_options;
  }

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;

    Block() = default;
    Block(const Block &copy) = delete;
    Block(Block &&move) = default;
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    buffer_info_.length = block_size;
    block.buffer = device_.newBuffer(buffer_info_);
    block.gpu_address = buffer_info_.gpu_address;
    return block;
  };

private:
  WMT::Device device_;
  WMTBufferInfo buffer_info_;
};

class StagingBufferBlockAllocator {
public:
  StagingBufferBlockAllocator(WMT::Device device, WMTResourceOptions block_options, bool placed_buffer = true) {
    device_ = device;
    buffer_info_ = block_options;
    // Spec 06 B2: in the i386 build the rings upstream lets Metal allocate use
    // DLL-allocated memory too; Metal's own has no 32-bit address.
    placed_buffer_ = placed_buffer || kI386Build;
  }

#ifdef DXMT_IOS
  /* Placed memory must be page-aligned for newBufferWithBytesNoCopy:, and
     iOS pages are 16 KB (util_placed_memory.hpp). */
  static void *
  allocatePlaced(size_t size) {
    return wsi::aligned_malloc(PlacedMemoryLength(size), kPlacedMemoryAlignment);
  }

  static void
  freePlaced(void *ptr) {
    wsi::aligned_free(ptr);
  }
#else
  static void *
  allocatePlaced(size_t size) {
    return malloc(size);
  }

  static void
  freePlaced(void *ptr) {
    free(ptr);
  }
#endif

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;
    void *mapped_address;

    ~Block() {
#ifdef DXMT_IOS
      // Spec 05 D6: the Metal buffer was made on this memory with
      // newBufferWithBytesNoCopy:, so release it before freeing the memory.
      buffer = WMT::Reference<WMT::Buffer>();
#endif
      if (mapped_address) {
        freePlaced(mapped_address);
        mapped_address = nullptr;
      }
    };

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      buffer = std::move(move.buffer);
      gpu_address = move.gpu_address;
      mapped_address = move.mapped_address;
      move.mapped_address = nullptr;
    };
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    block.mapped_address = placed_buffer_ ? allocatePlaced(block_size) : nullptr;
    WMTBufferInfo info;
    info.options = buffer_info_;
    info.memory.set(block.mapped_address);
#ifdef DXMT_IOS
    info.length = block.mapped_address ? PlacedMemoryLength(block_size) : block_size;
#else
    info.length = block_size;
#endif
    block.buffer = device_.newBuffer(info);
    block.gpu_address = info.gpu_address;
    return block;
  };

private:
  WMT::Device device_;
  WMTResourceOptions buffer_info_;
  bool placed_buffer_;
};

class HostBufferBlockAllocator {
public:
  class Block {
  public:
    void *ptr;

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      ptr = move.ptr;
      move.ptr = nullptr;
    };

    ~Block() {
      if (ptr) {
        free(ptr);
        ptr = nullptr;
      }
    };
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    block.ptr = malloc(block_size);
    return block;
  };
};

template <typename Allocator, size_t BlockSize, class mutex>
std::pair<typename Allocator::Block &, uint64_t>
RingBumpState<Allocator, BlockSize, mutex>::allocate(
    uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment
) {
  std::lock_guard<mutex> lock(mutex_);
  note_single_writer(); /* spec 07 */
  while (!fifo.empty()) {
    auto &latest = fifo.back();
    if ((align(latest.allocated_size, alignment) + size) > latest.total_size) {
      break;
    }
    latest.last_used_seq_id = seq_id;
    return suballocate(latest, size, alignment);
  }
  return suballocate(
      allocate_or_reuse_block(
          seq_id, coherent_id, std::max(size, BlockSize) // in case required size is larger than block size
      ),
      size, alignment
  );
};

template <typename Allocator, size_t BlockSize, class mutex>
void
RingBumpState<Allocator, BlockSize, mutex>::free_blocks(uint64_t coherent_id) {
  std::lock_guard<mutex> lock(mutex_);
  while (!fifo.empty()) {
    auto &front = fifo.front();
    if (front.last_used_seq_id > coherent_id)
      break;
    auto expired = (coherent_id - front.last_used_seq_id) > kStagingBlockLifetime ||
                   front.inc_time_to_live > kStagingBlockLifetime || coherent_id == -1ull;
    auto adhoc = is_adhoc(front);
    if (expired || adhoc) {
      // can be deallocated
      pop_front();
      continue;
    }
    front.inc_time_to_live++;
    break;
  }
};

template <typename Allocator, size_t BlockSize, class mutex>
RingBumpState<Allocator, BlockSize, mutex>::Allocation &
RingBumpState<Allocator, BlockSize, mutex>::allocate_or_reuse_block(
    uint64_t seq_id, uint64_t coherent_id, size_t block_size
) {
  while (!fifo.empty()) {
    auto &front = fifo.front();
    if (front.last_used_seq_id < coherent_id) {
      if (is_adhoc(front)) {
        pop_front();
        continue;
      }
#ifdef DXMT_IOS
      if (front.reusable_oversize && front.total_size < block_size) {
        // spec 05 D7: a finished oversized block too small for this request is
        // dropped instead of blocking the queue
        pop_front();
        continue;
      }
#endif
      if (front.total_size >= block_size) {
        front.last_used_seq_id = seq_id;
        front.allocated_size = 0;
        front.inc_time_to_live = 0;
        fifo.push(std::move(front));
        fifo.pop();
        return fifo.back();
      }
      WARN("forced to allocate new block of size ", block_size);
    }
    break;
  }
  bool reusable_oversize = false;
#ifdef DXMT_IOS
  if (block_size > BlockSize && block_size <= kOversizeReuseMaxBytes && kept_oversize_ < kOversizeReuseMaxBlocks &&
      RingOversizeReuseEnabled()) {
    reusable_oversize = true;
    kept_oversize_++;
  }
#endif
  fifo.push({
      .allocated_size = 0,
      .total_size = block_size,
      .last_used_seq_id = seq_id,
      .inc_time_to_live = 0,
      .block = allocator_.allocate(block_size),
      .reusable_oversize = reusable_oversize,
  });
  return fifo.back();
};

} // namespace dxmt