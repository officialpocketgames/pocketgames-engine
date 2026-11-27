#pragma once
// Changed for MYIOSDECK (spec 05 D6, D7; spec 06 A, B2, B3; spec 07 A2: LGPL-2.1-or-later blocks
// from dxmt-d3d9 v0.4-d3d9 (e8dd4c6), marked inline, see LICENSE-NOTES.md; spec 05 clarification 10:
// an LGPL-2.1-or-later block from steamac's DXMT patch 0002, marked inline). Upstream: DXMT a3a6e8f, MIT.
// Changed for MYIOSDECK (spec 33 B2: ring memory of the ios_wow_native build from the native heap).
// Changed for MYIOSDECK (spec 06 round 2: placed blocks counted as guest memory; a failed one dropped).
// Changed for MYIOSDECK (spec 05 item 90: each block's Metal buffer in the memory ledger, by ring; a
// finished one-off block freed wherever it is).
// Changed for MYIOSDECK (spec 06 round 3: host blocks from a free list, in guest memory in the i386
// build, checked).
// Changed for MYIOSDECK (spec 06 round 4: a ring's blocks may start small and double; its bytes).

#include "Metal.hpp"
#include "dxmt_mem_track.hpp" // MYIOSDECK spec 05 item 90
#include "log/log.hpp"
#include "thread.hpp"
#include "util_bitness.hpp"
#include "util_math.hpp"
#include "util_guest_memory.hpp" // MYIOSDECK spec 06 round 3: command blocks, every build
#include <cassert>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <list> // MYIOSDECK spec 05 item 90
#include <queue>
#ifdef DXMT_IOS
#include "util_ios_switch.hpp"
#include "util_placed_memory.hpp"
#include "wsi_platform.hpp"
#include "util_guest_memory.hpp" // MYIOSDECK spec 06 round 2
#endif

namespace dxmt {

// Spec 06 B3: the i386 build uses 8 MB blocks; the program, its data and every
// block share one 4 GB window.
constexpr size_t kStagingBlockSize = kI386Build ? 0x800000 : 0x2000000; // 8MB / 32MB
constexpr size_t kStagingBlockSizeForDeferredContext = 0x200000; // 2MB
constexpr size_t kStagingBlockLifetime = 300;

/* MYIOSDECK spec 06 round 4 (item 4): a deferred command list's command and staging blocks start
   at 64 KB in a 32-bit process and double up to kStagingBlockSizeForDeferredContext as the list
   records more; 64-bit processes keep 2 MB blocks. A game finishing ~1,900 lists before its first
   Present held a 2 MB block for each, however little it recorded. */
constexpr size_t kDeferredFirstBlockSizeI386 = 0x10000; // 64 KB
constexpr size_t
DeferredFirstBlockSize(bool i386) {
  return i386 ? kDeferredFirstBlockSizeI386 : kStagingBlockSizeForDeferredContext;
}

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
      fifo.push_back({
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

  /* MYIOSDECK spec 06 round 4 (item 4): new blocks start at `first` and double, up to BlockSize,
     each time the ring needs another; a request larger than the next size still gets a block of
     its own size. restartGrowth starts again from `first` (a deferred list being reset). The
     default, and `first` 0 or BlockSize, is upstream's fixed size. */
  void
  growFrom(size_t first) {
    std::lock_guard<mutex> lock(mutex_);
    first_block_size_ = (first && first < BlockSize) ? first : BlockSize;
    next_block_size_ = first_block_size_;
  }

  void
  restartGrowth() {
    std::lock_guard<mutex> lock(mutex_);
    next_block_size_ = first_block_size_;
  }

  /* the size the next new block will have, at least */
  size_t
  nextBlockSize() {
    std::lock_guard<mutex> lock(mutex_);
    return next_block_size_;
  }

  /* the bytes of the ring's blocks (a deferred list's command memory, for the backlog) */
  uint64_t
  blockBytes() {
    std::lock_guard<mutex> lock(mutex_);
    uint64_t bytes = 0;
    for (const auto &allocation : fifo)
      bytes += allocation.total_size;
    return bytes;
  }

  /* the ring's blocks (the host tests) */
  size_t
  blockCount() {
    std::lock_guard<mutex> lock(mutex_);
    return fifo.size();
  }

private:
  size_t first_block_size_ = BlockSize; // MYIOSDECK spec 06 round 4
  size_t next_block_size_ = BlockSize;  // MYIOSDECK spec 06 round 4

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
    fifo.pop_front();
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

  /* MYIOSDECK spec 05 item 90: a list (upstream: a queue), so that free_blocks can drop a finished
     one-off block from anywhere while references to the other blocks stay valid */
  std::list<Allocation> fifo;
  mutex mutex_;
  Allocator allocator_;
};

class GpuPrivateBufferBlockAllocator {
public:
  /* MYIOSDECK spec 05 item 90: `name` (static) says which ring a block is of in the ledger */
  GpuPrivateBufferBlockAllocator(
      WMT::Device device, WMTResourceOptions block_options, const char *name = "copy temporaries"
  ) {
    device_ = device;
    buffer_info_.memory.set(nullptr);
    buffer_info_.options = block_options;
    name_ = name;
  }

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;
    memledger::Entry mem_record; // MYIOSDECK spec 05 item 90

    Block() = default;
    Block(const Block &copy) = delete;
    Block(Block &&move) = default;
    ~Block() {
      mem_record.reset(); // item 90: the record goes with the buffer
      buffer = WMT::Reference<WMT::Buffer>();
    }
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    buffer_info_.length = block_size;
    block.buffer = device_.newBuffer(buffer_info_);
    block.gpu_address = buffer_info_.gpu_address;
    if (block.buffer) // MYIOSDECK spec 05 item 90
      block.mem_record = memledger::Entry(memledger::Add(
          memledger::BufferRecord(memledger::Kind::Staging, block_size, buffer_info_.options, name_)
      ));
    return block;
  };

private:
  WMT::Device device_;
  WMTBufferInfo buffer_info_;
  const char *name_;
};

class StagingBufferBlockAllocator {
public:
  StagingBufferBlockAllocator(
      WMT::Device device, WMTResourceOptions block_options, bool placed_buffer = true,
      const char *name = "staging" // MYIOSDECK spec 05 item 90: the ring, in the ledger
  ) {
    device_ = device;
    buffer_info_ = block_options;
    name_ = name;
    // Spec 06 B2: in the i386 build the rings upstream lets Metal allocate use
    // DLL-allocated memory too; Metal's own has no 32-bit address.
    // MYIOSDECK spec 33 B2: not in the ios_wow_native build, whose rings the
    // program never sees.
    placed_buffer_ = placed_buffer || (kI386Build && !kIosWowNative);
  }

#ifdef DXMT_IOS
  /* Placed memory must be page-aligned for newBufferWithBytesNoCopy:, and
     iOS pages are 16 KB (util_placed_memory.hpp). */
  static void *
  allocatePlaced(size_t size) {
#ifdef DXMT_IOS_WOW_NATIVE
    /* MYIOSDECK spec 33 B2: memory only Metal and the frontend touch: the native heap */
    return ios_wow_native::allocHost(PlacedMemoryLength(size), kPlacedMemoryAlignment);
#else
    /* MYIOSDECK spec 06 round 2: counted as guest memory; null when there is none left */
    return guest_memory::Alloc(
        PlacedMemoryLength(size), kPlacedMemoryAlignment, guest_memory::Use::Staging, "a staging ring block"
    );
#endif
  }

  static void
  freePlaced(void *ptr, size_t size) {
#ifdef DXMT_IOS_WOW_NATIVE
    (void)size;
    wsi::aligned_free(ptr);
#else
    guest_memory::Free(ptr, PlacedMemoryLength(size), guest_memory::Use::Staging);
#endif
  }
#else
  static void *
  allocatePlaced(size_t size) {
    return malloc(size);
  }

  static void
  freePlaced(void *ptr, size_t) {
    free(ptr);
  }
#endif

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;
    void *mapped_address;
    /* LGPL-2.1-or-later, from steamac's DXMT patch 0002 (StonkGuy): the memory placed
       under the buffer, freed with the block. mapped_address is where newBuffer says
       the contents are, which a unix side may back with other memory. */
    void *placed_address;
    size_t placed_size = 0; // MYIOSDECK spec 06 round 2: for the guest-memory ledger
    bool placed_failed = false;
    memledger::Entry mem_record; // MYIOSDECK spec 05 item 90

    /* MYIOSDECK spec 06 round 2: its placed memory couldn't be had */
    bool
    failed() const {
      return placed_failed;
    }

    ~Block() {
      mem_record.reset(); // MYIOSDECK spec 05 item 90: the record goes with the buffer
#ifdef DXMT_IOS
      // Spec 05 D6: the Metal buffer was made on this memory with
      // newBufferWithBytesNoCopy:, so release it before freeing the memory.
      buffer = WMT::Reference<WMT::Buffer>();
#endif
      if (placed_address) {
        freePlaced(placed_address, placed_size);
        placed_address = nullptr;
      }
      mapped_address = nullptr;
    };

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      buffer = std::move(move.buffer);
      gpu_address = move.gpu_address;
      mapped_address = move.mapped_address;
      placed_address = move.placed_address;
      placed_size = move.placed_size;
      placed_failed = move.placed_failed;
      mem_record = std::move(move.mem_record); // MYIOSDECK spec 05 item 90
      move.mapped_address = nullptr;
      move.placed_address = nullptr;
    };
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    block.placed_address = placed_buffer_ ? allocatePlaced(block_size) : nullptr;
    block.placed_size = block_size;
    block.placed_failed = placed_buffer_ && !block.placed_address; // MYIOSDECK spec 06 round 2
    WMTBufferInfo info;
    info.options = buffer_info_;
    info.memory.set(block.placed_address);
#ifdef DXMT_IOS
    info.length = block.placed_address ? PlacedMemoryLength(block_size) : block_size;
#else
    info.length = block_size;
#endif
    block.buffer = device_.newBuffer(info);
    block.gpu_address = info.gpu_address;
    /* CPU writes go where newBuffer says the contents are, as BufferAllocation does */
    block.mapped_address = placed_buffer_ ? info.memory.get() : nullptr;
    /* end of LGPL block */
    if (block.buffer) // MYIOSDECK spec 05 item 90
      block.mem_record = memledger::Entry(
          memledger::Add(memledger::BufferRecord(memledger::Kind::Staging, info.length, buffer_info_, name_))
      );
    return block;
  };

private:
  WMT::Device device_;
  WMTResourceOptions buffer_info_;
  bool placed_buffer_;
  const char *name_; // MYIOSDECK spec 05 item 90
};

/* MYIOSDECK spec 06 round 3 (item 2): the command rings' host blocks (the command queue's, each
   deferred command list's, the reference trackers') come from a free list when one of the size
   is there, and go back to it when their ring lets them go; only the bytes past
   kHostBlockKeepBytes are freed. Upstream mallocs a block each time and frees it with its ring:
   a deferred command list, reset after each use, made a fresh 2 MB block every time it was
   recorded again.
   In a guest window (the i386 build) a block is guest memory, counted under "commands" and
   checked (guest_memory::Alloc). A block may then come back without memory (failed()): its
   users wait for the GPU and try again, or drop what they were writing; never a write through
   null. g_host_blocks_in_guest lets the host benchmark take that path in a 64-bit build. */
inline bool g_host_blocks_in_guest = guest_memory::kGuestWindowHeap;
constexpr size_t kHostBlockKeepBytes = kI386Build ? (16ull << 20) : (64ull << 20);

class HostBlockPool {
public:
  struct Entry {
    void *ptr;
    size_t size;
    bool guest;
  };

  static Entry
  take(size_t size) {
    List &l = list();
    {
      std::lock_guard<dxmt::mutex> lock(l.mutex);
      for (size_t i = 0; i < l.blocks.size(); i++) {
        if (l.blocks[i].size == size && l.blocks[i].guest == g_host_blocks_in_guest) {
          Entry e = l.blocks[i];
          l.blocks[i] = l.blocks.back();
          l.blocks.pop_back();
          l.bytes -= size;
          return e;
        }
      }
    }
    Entry e{nullptr, size, g_host_blocks_in_guest};
    e.ptr = e.guest ? guest_memory::Alloc(size, 64, guest_memory::Use::Commands, "a command block") : malloc(size);
    return e;
  }

  static void
  give(const Entry &e) {
    if (!e.ptr)
      return;
    List &l = list();
    {
      std::lock_guard<dxmt::mutex> lock(l.mutex);
      if (l.bytes + e.size <= kHostBlockKeepBytes) {
        l.blocks.push_back(e);
        l.bytes += e.size;
        return;
      }
    }
    release(e);
  }

  /* the free list's bytes, and every block in it released (the host tests) */
  static size_t
  keptBytes() {
    List &l = list();
    std::lock_guard<dxmt::mutex> lock(l.mutex);
    return l.bytes;
  }

  static void
  releaseKept() {
    List &l = list();
    std::vector<Entry> blocks;
    {
      std::lock_guard<dxmt::mutex> lock(l.mutex);
      blocks.swap(l.blocks);
      l.bytes = 0;
    }
    for (const Entry &e : blocks)
      release(e);
  }

private:
  struct List {
    dxmt::mutex mutex;
    std::vector<Entry> blocks;
    size_t bytes = 0;
  };

  static List &
  list() {
    static List *l = new List(); // never destroyed: rings may give blocks back during exit
    return *l;
  }

  static void
  release(const Entry &e) {
    if (e.guest)
      guest_memory::Free(e.ptr, e.size, guest_memory::Use::Commands);
    else
      free(e.ptr);
  }
};

class HostBufferBlockAllocator {
public:
  class Block {
  public:
    void *ptr = nullptr;
    size_t size = 0;    // MYIOSDECK spec 06 round 3: for the free list
    bool guest = false; // MYIOSDECK spec 06 round 3: guest memory (released through the ledger)

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      ptr = move.ptr;
      size = move.size;
      guest = move.guest;
      move.ptr = nullptr;
    };

    ~Block() {
      if (ptr) {
        HostBlockPool::give({ptr, size, guest}); // MYIOSDECK spec 06 round 3 (upstream: free)
        ptr = nullptr;
      }
    };

    /* MYIOSDECK spec 06 round 3: its memory couldn't be had (guest memory only) */
    bool
    failed() const {
      return !ptr;
    }
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    HostBlockPool::Entry e = HostBlockPool::take(block_size); // MYIOSDECK spec 06 round 3
    block.ptr = e.ptr;
    block.size = e.size;
    block.guest = e.guest;
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
  /* MYIOSDECK spec 06 round 4 (item 4): next_block_size_ is BlockSize unless the ring grows */
  auto &block = allocate_or_reuse_block(
      seq_id, coherent_id, std::max(size, next_block_size_) // in case required size is larger than block size
  );
  if (next_block_size_ < BlockSize)
    next_block_size_ = std::min(next_block_size_ * 2, BlockSize);
  return suballocate(block, size, alignment);
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
  /* MYIOSDECK spec 05 item 90 (round 20's second fix): a block made for one request larger than
     the ring's blocks goes as soon as the GPU has finished with it, wherever it is. Upstream
     dropped it only on reaching the front, and a finished block kept for reuse holds the front
     until it is reused or kStagingBlockLifetime chunks pass, so a run of large uploads could
     pile such blocks up behind it. A block a caller may still be writing belongs to a chunk the
     GPU hasn't finished, so it is never one of these. */
  for (auto it = fifo.begin(); it != fifo.end();) {
    if (is_adhoc(*it) && it->last_used_seq_id <= coherent_id)
      it = fifo.erase(it);
    else
      ++it;
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
        fifo.splice(fifo.end(), fifo, fifo.begin()); // item 90: the node moves, not the block
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
  fifo.push_back({
      .allocated_size = 0,
      .total_size = block_size,
      .last_used_seq_id = seq_id,
      .inc_time_to_live = 0,
      .block = allocator_.allocate(block_size),
      .reusable_oversize = reusable_oversize,
  });
  /* MYIOSDECK spec 06 round 2: a block whose placed memory couldn't be had serves this one
     request (its caller sees the null buffer) and is dropped at the next free_blocks, never
     suballocated again */
  if constexpr (requires(typename Allocator::Block &b) { b.failed(); }) {
    if (fifo.back().block.failed()) {
#ifdef DXMT_IOS
      if (fifo.back().reusable_oversize)
        kept_oversize_--;
#endif
      fifo.back().reusable_oversize = false;
      fifo.back().total_size = 0;
    }
  }
  return fifo.back();
};

} // namespace dxmt