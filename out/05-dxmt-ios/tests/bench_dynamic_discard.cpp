// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Host benchmark and test for Map(WRITE_DISCARD) on a dynamic constant buffer:
// N cycles per frame of discard-map, a memcpy of 4-64 KB, unmap, bind and draw
// on one 64 KB D3D11_USAGE_DYNAMIC buffer, as an OpenGL ES layer on D3D11 does.
//
// It runs DXMT's own Buffer / BufferAllocation / DynamicBuffer
// (dxmt/src/dxmt/dxmt_buffer.cpp, dxmt_dynamic.cpp), the AllocationRefTracking a
// command chunk uses to keep what it read alive, and the discard policy
// (dxmt_dynamic_policy.hpp), replaying the immediate context's Map sequence
// (d3d11_context_imm.cpp MapDynamicBuffer / BoundDiscardMemory) in recording
// order. Metal is faked: MTLDevice_newBuffer hands out heap memory and counts
// it, so the run reports how many Metal buffers were created and the most
// buffer memory held at once. The GPU is a model: it finishes committed chunks
// `gpu_lag` chunks behind the CPU, or at once when the CPU waits for one; a
// finished chunk checks that every draw in it still finds the bytes it was
// recorded with (a slice reused too early would fail that).
//
// usage: bench_dynamic_discard [--check] [frames] [cycles-per-frame] [mode]
//   mode new (default): this tree's behaviour
//   mode old: one slice per allocation, no implicit commit, no in-flight cap
//             (the code before this change; memory is aliased so it fits)
//   --check: fewer cycles, exit non-zero if a bound or a check fails
//
// Spec 06 round 4 (item 4), with --check: a deferred list's ring with 64 KB first blocks that
// double, 2,000 lists executed before any Present under the backlog's commits, and the wait at
// the command-memory mark (dxmt_cmdlist_backlog.hpp), all on DXMT's own RingBumpState.

#include "dxmt_buffer.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_dynamic_policy.hpp"
#include "dxmt_format.hpp"
#include "wsi_platform.hpp"
#include "util_guest_memory.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_cmdlist_backlog.hpp" // spec 06 round 4

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

/* ---- fake Metal ---------------------------------------------------------- */

namespace {

struct FakeMetal {
  std::map<obj_handle_t, std::pair<void *, uint64_t>> buffers;
  obj_handle_t next_handle = 0x1000;
  uint64_t live_bytes = 0;
  uint64_t peak_bytes = 0;
  uint64_t new_buffers = 0;
  bool alias = false; // old mode: every buffer shares one block, so 2 GB a frame fits
  bool refuse_without_memory = false; // spec 06 round 2: as the 32-bit unix side (B4), refuse a
                                      // CPU-visible buffer that brings no memory
  void *alias_block = nullptr;
  uint64_t alias_size = 0;
};

FakeMetal g_metal;

} // namespace

extern "C" {

obj_handle_t
MTLDevice_newBuffer(obj_handle_t, struct WMTBufferInfo *info) {
  void *mem = info->memory.get();
  bool owned = false;
  if (!mem && g_metal.refuse_without_memory)
    return 0;
  if (!mem) {
    if (g_metal.alias) {
      if (g_metal.alias_size < info->length) {
        std::free(g_metal.alias_block);
        g_metal.alias_block = std::calloc(1, info->length);
        g_metal.alias_size = info->length;
      }
      mem = g_metal.alias_block;
    } else {
      mem = std::calloc(1, info->length); // Metal hands out zeroed memory
      owned = true;
    }
    info->memory.set(mem);
  }
  obj_handle_t h = g_metal.next_handle++;
  g_metal.buffers[h] = {owned ? mem : nullptr, info->length};
  info->gpu_address = 0x100000000ull + uint64_t(h) * 0x1000000ull;
  g_metal.new_buffers++;
  g_metal.live_bytes += info->length;
  if (g_metal.live_bytes > g_metal.peak_bytes)
    g_metal.peak_bytes = g_metal.live_bytes;
  return h;
}

void
NSObject_release(obj_handle_t obj) {
  auto it = g_metal.buffers.find(obj);
  if (it == g_metal.buffers.end())
    return;
  g_metal.live_bytes -= it->second.second;
  std::free(it->second.first);
  g_metal.buffers.erase(it);
}

void
NSObject_retain(obj_handle_t) {}

obj_handle_t
MTLBuffer_newTexture(obj_handle_t, struct WMTTextureInfo *, uint64_t, uint64_t) {
  return 0;
}

bool
MTLDevice_supportsFamily(obj_handle_t, enum WMTGPUFamily) {
  return false;
}

/* Spec 05 clarification 10: Allocation::decRef takes an allocation out of the queue's
   residency set (steamac's DXMT patch 0023); the benchmark runs without a set. */
void
MTLResidencySet_removeAllocations(obj_handle_t, const obj_handle_t *, uint64_t) {}

void
MTLResidencySet_addAllocations(obj_handle_t, const obj_handle_t *, uint64_t) {}

void
MTLResidencySet_commit(obj_handle_t) {}

} // extern "C"

namespace dxmt {

uint32_t
MTLGetTexelSize(WMTPixelFormat) {
  return 4;
}

Rc<TextureAllocation>
Texture::allocate(Flags<TextureAllocationFlag>) {
  std::abort(); // not used by buffers
}

bool g_fail_alloc = false; // spec 06 round 2: the guest window is full
uint64_t g_guest_allocs = 0; // spec 06 round 3: guest allocations made (not taken from a free list)

/* spec 06 round 2: the staging ring's warnings (log.cpp in the DLL) */
void
Logger::warn(const std::string &message) {
  std::printf("warn: %s\n", message.c_str());
}

/* spec 06 round 3: the rings' switch (DXMT_RING_OVERSIZE_REUSE; util_env.cpp in the DLL): unset */
namespace env {
std::string
getEnvVar(const char *) {
  return "";
}
} // namespace env

/* spec 06 round 2: the guest-memory lines (util_guest_memory_log.cpp in the DLL) */
namespace guest_memory {
void
Write(const std::string &line, bool) {
  std::printf("%s\n", line.c_str());
}
} // namespace guest_memory

namespace wsi {
void *
aligned_malloc(size_t size, size_t alignment) {
  if (g_fail_alloc)
    return nullptr;
  g_guest_allocs++;
  return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}
void
aligned_free(void *ptr) {
  std::free(ptr);
}
} // namespace wsi

} // namespace dxmt

/* ---- the context and GPU model -------------------------------------------- */

using namespace dxmt;

namespace {

struct DrawCheck {
  const uint64_t *slice;
  uint64_t tag;
};

struct Chunk {
  uint64_t seq;
  AllocationRefTracking retained; // what a real chunk keeps alive until the GPU is done
  std::vector<DrawCheck> draws;
  std::vector<void *> storage; // more room for `retained` (a real chunk takes it from its CPU heap)

  void
  retain(Allocation *allocation) {
    while (!retained.track(allocation)) {
      constexpr size_t kStorage = 64 * 1024;
      void *s = std::malloc(kStorage);
      retained.addStorage(s, kStorage);
      storage.push_back(s);
    }
  }

  ~Chunk() {
    retained.clear();
    for (void *s : storage)
      std::free(s);
  }
};

Chunk *
newChunk(uint64_t seq) {
  Chunk *c = new Chunk();
  c->seq = seq;
  return c;
}

struct Model {
  bool new_mode = true;
  unsigned gpu_lag = 2; // chunks the GPU runs behind the CPU
  uint32_t max_latency = 3;

  uint64_t current_seq = 1;
  uint64_t coherent_seq = 0;
  DynamicDiscardTracker tracker;
  std::deque<Chunk *> committed;
  Chunk *chunk = newChunk(1);
  std::vector<uint64_t> present_seqs;

  uint64_t commits = 0;
  uint64_t logged_commits = 0; // implicit commits that write a [dxmt] line
  uint64_t waits = 0;
  uint64_t bad_reads = 0;

  explicit Model(bool new_mode_) :
      new_mode(new_mode_),
      tracker(new_mode_ ? kDefaultDynamicCommitBytes : 0, new_mode_ ? kDefaultDynamicInflightBytes : 0) {}

  void
  finishOne() {
    Chunk *c = committed.front();
    committed.pop_front();
    if (new_mode)
      for (auto &d : c->draws)
        if (*d.slice != d.tag)
          bad_reads++;

    coherent_seq = c->seq;
    delete c;
  }

  void
  waitFor(uint64_t seq) { // WaitCPUFence
    waits++;
    while (coherent_seq < seq && !committed.empty())
      finishOne();
  }

  void
  commit() { // ImmediateContext::Commit -> CommandQueue::CommitCurrentChunk
    tracker.onCommit(current_seq);
    committed.push_back(chunk);
    commits++;
    current_seq++;
    chunk = newChunk(current_seq);
    while (committed.size() > gpu_lag)
      finishOne();
  }

  void
  present() { // swapchain Present + CommandQueue::PresentBoundary
    present_seqs.push_back(current_seq);
    commit();
    size_t frame = present_seqs.size();
    if (frame > max_latency)
      waitFor(present_seqs[frame - 1 - max_latency]);
  }

  /* d3d11_context_imm.cpp MapDynamicBuffer with BoundDiscardMemory; the
     EmitST bodies run in place (they run in this order on the encode thread). */
  void *
  mapDiscard(DynamicBuffer &dyn) {
    if (tracker.commitDue()) {
      if (DynamicDiscardTracker::logImplicitCommit(tracker.noteImplicitCommit()))
        logged_commits++;
      commit();
    }
    if (uint64_t seq = tracker.waitSeq(coherent_seq)) {
      waitFor(seq);
      tracker.retire(coherent_seq);
    }
    tracker.noteDiscard(DynamicSliceBytes(dyn.buffer->length()));
    if (auto next_sub = dyn.nextSuballocation()) {
      dyn.immediateName()->useSuballocation(next_sub);
    } else {
      // spec 06 round 2 (d3d11_context_imm.cpp WaitForGuestRetire): at the cap, wait for the oldest
      if (uint64_t seq = dyn.guestWaitSeq(coherent_seq)) {
        if (seq >= current_seq)
          commit();
        waitFor(seq);
        tracker.retire(coherent_seq);
      }
      auto fresh_allocation = dyn.allocate(coherent_seq, nullptr, new_mode ? current_seq : 0);
      if (!fresh_allocation->mappedMemory(0))
        return nullptr; // Map returns E_OUTOFMEMORY
      dyn.updateImmediateName(current_seq, std::move(fresh_allocation), 0, false);
      auto allocation = dyn.immediateName();
      allocation->useSuballocation(0);
      auto previous = dyn.buffer->rename(std::move(allocation));
      (void)previous;
    }
    return dyn.immediateMappedMemory();
  }

  /* A draw that reads the bound constant buffer: the encode thread's access()
     keeps the allocation for the chunk and reads the current slice. */
  void
  draw(Buffer &buffer, uint64_t tag) {
    BufferAllocation *allocation = buffer.current();
    chunk->retain(allocation);
    if (new_mode)
      chunk->draws.push_back({static_cast<const uint64_t *>(allocation->mappedMemory(allocation->currentSuballocation())), tag});
  }
};

/* ---- spec 05 clarification 67: a deferred context's discards -------------------------------- */

struct RingFrame {
  uint64_t allocations = 0; // new allocations the list asked for
  uint64_t new_buffers = 0; // Metal buffers made
  uint32_t last_slices = 0; // slices of the last allocation handed out
  bool handed_out_smaller = false;
};

/* `frames` frames, each recording one command list with `discards` Map(WRITE_DISCARD)s of a
   `length`-byte dynamic buffer the way d3d11_context_def.cpp's MapDynamicBuffer does; the list
   runs as chunk f + 1 and is released (its allocations back in the pool) two frames later, when
   the GPU has finished it. `ring` is DXMT_D11_DEFERRED_RING. */
std::vector<RingFrame>
RunDeferredRing(bool ring, uint32_t length, unsigned frames, unsigned discards) {
  Rc<Buffer> buffer = new Buffer(length, WMT::Device{});
  Flags<BufferAllocationFlag> flags;
  flags.set(BufferAllocationFlag::CpuWriteCombined);
  flags.set(BufferAllocationFlag::SuballocateFromOnePage);
  auto initial = buffer->rename(buffer->allocate(flags));
  (void)initial;
  Rc<DynamicBuffer> dyn = new DynamicBuffer(buffer.ptr(), flags);
  std::deque<std::pair<uint64_t, std::vector<Rc<BufferAllocation>>>> lists; // (run as chunk, allocations)
  std::vector<RingFrame> result;
  uint64_t coherent = 0;
  for (unsigned f = 0; f < frames; f++) {
    // the list of two frames ago is released: its allocations go back to the pool
    while (!lists.empty() && lists.front().first + 2 <= f + 1) {
      coherent = lists.front().first;
      for (auto &allocation : lists.front().second)
        dyn->recycle(lists.front().first, std::move(allocation));
      lists.pop_front();
    }
    RingFrame frame;
    const uint64_t buffers_before = g_metal.new_buffers;
    std::vector<Rc<BufferAllocation>> held;
    BufferAllocation *current = nullptr;
    uint32_t sub = 0;
    for (unsigned i = 0; i < discards; i++) {
      if (current && current->hasSuballocatoin(sub + 1)) {
        sub++;
        continue;
      }
      bool fresh = false;
      Rc<BufferAllocation> allocation = ring ? dyn->allocateRing(coherent, &fresh, 0, current != nullptr)
                                             : dyn->allocate(coherent, &fresh);
      frame.allocations++;
      if (!fresh && current && allocation->suballocationCount() < current->suballocationCount())
        frame.handed_out_smaller = true;
      frame.last_slices = allocation->suballocationCount();
      current = allocation.ptr();
      sub = 0;
      held.push_back(std::move(allocation));
    }
    frame.new_buffers = g_metal.new_buffers - buffers_before;
    lists.emplace_back(uint64_t(f + 1), std::move(held));
    result.push_back(frame);
  }
  return result;
}

int
CheckDeferredRing() {
  int failures = 0;
  // 4,000 discards of a 256-byte buffer a frame, 16 slices to a 4 KB page
  auto on = RunDeferredRing(true, 256, 8, 4000);
  for (unsigned f = 4; f < on.size(); f++) {
    if (on[f].new_buffers != 0 || on[f].allocations != 1) {
      std::printf("FAIL: ring on, frame %u: %llu allocations, %llu new buffers\n", f + 1,
                  (unsigned long long)on[f].allocations, (unsigned long long)on[f].new_buffers);
      failures++;
    }
  }
  for (auto &frame : on)
    if (frame.handed_out_smaller) {
      std::printf("FAIL: ring on: a smaller finished allocation was handed out\n");
      failures++;
      break;
    }
  auto off = RunDeferredRing(false, 256, 8, 4000);
  for (unsigned f = 4; f < off.size(); f++) {
    if (off[f].allocations != 250 || off[f].new_buffers < 180 || off[f].new_buffers > 190) {
      std::printf("FAIL: ring off, frame %u: %llu allocations, %llu new buffers (about 250 and 186)\n", f + 1,
                  (unsigned long long)off[f].allocations, (unsigned long long)off[f].new_buffers);
      failures++;
    }
  }
  // discarded once per list: the slice count stays the page's 16
  auto once = RunDeferredRing(true, 256, 8, 1);
  for (unsigned f = 0; f < once.size(); f++)
    if (once[f].last_slices != 16) {
      std::printf("FAIL: discarded once per list, frame %u: %u slices\n", f + 1, once[f].last_slices);
      failures++;
    }
  std::printf("  deferred ring (clarification 67): frame 5 on %llu allocation(s), %llu new buffers; off %llu and %llu\n",
              (unsigned long long)on[4].allocations, (unsigned long long)on[4].new_buffers,
              (unsigned long long)off[4].allocations, (unsigned long long)off[4].new_buffers);
  return failures;
}

} // namespace

/* ---- spec 06 round 2: a 32-bit program's guest window --------------------------------------- */

/* As the i386 build runs: the per-resource cap on, placed memory counted as guest memory, the
   unix side refusing a CPU-visible buffer without memory. The GPU model never catches up on its
   own (it finishes a chunk only when the CPU waits for it), the worst case for memory. */
int
CheckGuestWindow() {
  int failures = 0;
  auto fail = [&](const char *what) {
    std::printf("FAIL guest window: %s\n", what);
    failures++;
  };
  g_dynamic_guest_cap = true;
  g_metal.refuse_without_memory = true;
  guest_memory::Ledger &ledger = guest_memory::ProcessLedger();
  constexpr uint32_t k2MB = 2u << 20;
  const uint64_t placed_2mb = PlacedMemoryLength(k2MB);

  /* 2000 Map(DISCARD)/Unmap on one 2 MB dynamic texture (its subresource buffer, CPU-placed as in
     the i386 build, d3d11_texture_dynamic.cpp), a draw reading each */
  {
    Rc<Buffer> buffer = new Buffer(k2MB, WMT::Device{});
    Flags<BufferAllocationFlag> flags;
    flags.set(BufferAllocationFlag::CpuWriteCombined, BufferAllocationFlag::SuballocateFromOnePage,
              BufferAllocationFlag::CpuPlaced);
    auto initial = buffer->rename(buffer->allocate(flags));
    (void)initial;
    Rc<DynamicBuffer> dyn = new DynamicBuffer(buffer.ptr(), flags);
    Model m(true);
    m.gpu_lag = 1u << 30;
    uint64_t peak = 0;
    for (unsigned i = 0; i < 2000; i++) {
      auto *p = static_cast<uint64_t *>(m.mapDiscard(*dyn));
      if (!p) {
        fail("a discard got no memory");
        break;
      }
      *p = 0x6000000ull + i;
      m.draw(*buffer, 0x6000000ull + i);
      peak = std::max(peak, ledger.snapshot().bytes[unsigned(guest_memory::Use::Maps)]);
    }
    const uint64_t bound = (kGuestDynamicAllocations + 1) * placed_2mb;
    std::printf("guest window: 2000 discards of a 2 MB texture: peak %.1f MiB of maps (bound %.1f), %llu waits\n",
                peak / 1048576.0, bound / 1048576.0, (unsigned long long)m.waits);
    if (peak > bound)
      fail("map memory above the bound");
    if (m.bad_reads)
      fail("a draw read another discard's bytes");
    m.commit(); // the chunk being recorded too, so nothing keeps an allocation
    while (!m.committed.empty())
      m.finishOne();
  }
  if (ledger.snapshot().bytes[unsigned(guest_memory::Use::Maps)] != 0)
    fail("map memory left after the texture was released");

  /* 2000 UpdateSubresource calls of 2 MB: the staging ring with the context's bound
     (BoundStagingMemory: the discard tracker's commit and in-flight rules) */
  {
    RingBumpState<StagingBufferBlockAllocator> ring(
        StagingBufferBlockAllocator(WMT::Device{}, WMTResourceStorageModeShared, true)
    );
    Model m(true);
    m.gpu_lag = 1u << 30;
    uint64_t peak = 0;
    for (unsigned i = 0; i < 2000; i++) {
      if (m.tracker.commitDue()) {
        m.tracker.noteImplicitCommit();
        m.commit();
      }
      if (uint64_t seq = m.tracker.waitSeq(m.coherent_seq)) {
        m.waitFor(seq);
        m.tracker.retire(m.coherent_seq);
      }
      m.tracker.noteDiscard(k2MB);
      ring.free_blocks(m.coherent_seq); // as the queue does when chunks complete
      auto [block, offset] = ring.allocate(m.current_seq, m.coherent_seq, k2MB, 16);
      if (!block.buffer || !block.mapped_address) {
        fail("an update got no staging memory");
        break;
      }
      std::memset(static_cast<char *>(block.mapped_address) + offset, int(i), 64);
      peak = std::max(peak, ledger.snapshot().bytes[unsigned(guest_memory::Use::Staging)]);
    }
    const uint64_t bound = kDefaultDynamicInflightBytes + kDefaultDynamicCommitBytes + 2 * kStagingBlockSize;
    std::printf("guest window: 2000 UpdateSubresource of 2 MB: peak %.1f MiB of staging (bound %.1f)\n",
                peak / 1048576.0, bound / 1048576.0);
    if (peak > bound)
      fail("staging memory above the bound");
    m.commit(); // the chunk being recorded too, so nothing keeps an allocation
    while (!m.committed.empty())
      m.finishOne();
    ring.free_blocks(-1ull);
  }

  /* a failed guest allocation: the map fails (E_OUTOFMEMORY), the texture keeps its memory, the
     calling thread's failure count moves */
  {
    Rc<Buffer> buffer = new Buffer(k2MB, WMT::Device{});
    Flags<BufferAllocationFlag> flags;
    flags.set(BufferAllocationFlag::CpuWriteCombined, BufferAllocationFlag::SuballocateFromOnePage,
              BufferAllocationFlag::CpuPlaced);
    auto initial = buffer->rename(buffer->allocate(flags));
    (void)initial;
    Rc<DynamicBuffer> dyn = new DynamicBuffer(buffer.ptr(), flags);
    Model m(true);
    void *before = dyn->immediateMappedMemory();
    const uint64_t failures_before = guest_memory::ThreadFailures();
    g_fail_alloc = true;
    void *mapped = m.mapDiscard(*dyn);
    g_fail_alloc = false;
    if (mapped)
      fail("a discard without memory still mapped");
    if (dyn->immediateMappedMemory() != before)
      fail("a failed discard replaced the texture's memory");
    if (guest_memory::ThreadFailures() != failures_before + 1)
      fail("the failure wasn't counted");
    if (!m.mapDiscard(*dyn))
      fail("the next discard, with memory again, failed");
    m.commit(); // the chunk being recorded too, so nothing keeps an allocation
    while (!m.committed.empty())
      m.finishOne();
  }

  g_dynamic_guest_cap = false;
  g_metal.refuse_without_memory = false;
  return failures;
}

/* spec 06 round 3 (item 2): the command blocks of a 32-bit program's deferred command lists, 1000
   frames on a GPU that finishes a chunk only when the CPU waits for it. Each frame records one
   list of about 3 MB of commands (two 2 MB blocks of its ring, as MTLD3D11CommandList's), the
   immediate context executes it in the frame's chunk, which holds it until the GPU has finished
   (then Reset gives its blocks back), and Present waits for the frame kI386MaxLatency (3) before.
   Blocks are guest memory here, as in the i386 build. Then a block that can't be had. */
int
CheckCommandBlocks() {
  int failures = 0;
  auto fail = [&](const char *what) {
    std::printf("FAIL command blocks: %s\n", what);
    failures++;
  };
  constexpr uint64_t kMB = 1ull << 20;
  constexpr size_t kListBlock = 0x200000; // kStagingBlockSizeForDeferredContext
  using ListRing = RingBumpState<HostBufferBlockAllocator, kListBlock, dxmt::null_mutex>;
  struct List {
    ListRing ring{HostBufferBlockAllocator{}};
    uint64_t seq = 0;
  };
  const bool was_guest = g_host_blocks_in_guest;
  g_host_blocks_in_guest = true;
  HostBlockPool::releaseKept();
  guest_memory::Ledger &ledger = guest_memory::ProcessLedger();
  const uint64_t commands_before = ledger.snapshot().bytes[unsigned(guest_memory::Use::Commands)];

  std::vector<std::unique_ptr<List>> idle;
  std::deque<std::unique_ptr<List>> in_flight;
  auto recycle = [&](uint64_t coherent) { // MTLD3D11CommandList::Recycle once its chunk is done
    while (!in_flight.empty() && in_flight.front()->seq <= coherent) {
      in_flight.front()->ring.free_blocks(~0ull);
      idle.push_back(std::move(in_flight.front()));
      in_flight.pop_front();
    }
  };
  {
    Model m(true);
    m.gpu_lag = 1u << 30;
    m.max_latency = 3; // CommandQueue::kI386MaxLatency
    uint64_t allocs_after_warmup = 0, peak = 0;
    size_t most_in_flight = 0;
    for (unsigned f = 0; f < 1000; f++) {
      if (f == 20)
        allocs_after_warmup = g_guest_allocs;
      recycle(m.coherent_seq);
      std::unique_ptr<List> list;
      if (idle.empty()) {
        list = std::make_unique<List>();
      } else {
        list = std::move(idle.back());
        idle.pop_back();
      }
      for (size_t recorded = 0; recorded < 3 * kMB;) {
        size_t n = 1024 + (recorded * 7919u) % 5120; // commands of 1-6 KB
        auto [block, offset] = list->ring.allocate(1, 0, n, 16);
        if (!block.ptr) {
          fail("a command got no memory");
          break;
        }
        std::memset(static_cast<char *>(block.ptr) + offset, int(f), n);
        recorded += n;
      }
      list->seq = m.current_seq; // executed in this chunk, which holds it
      in_flight.push_back(std::move(list));
      most_in_flight = std::max(most_in_flight, in_flight.size());
      m.present();
      peak = std::max(peak, ledger.snapshot().bytes[unsigned(guest_memory::Use::Commands)] - commands_before);
    }
    const uint64_t fresh = g_guest_allocs - allocs_after_warmup;
    const uint64_t bound = (m.max_latency + 3) * 2 * kListBlock;
    std::printf("command blocks: 1000 frames of a 3 MB deferred list: peak %.1f MiB (bound %.1f), %zu lists at "
                "most in flight, %llu guest allocations after frame 20, %.1f MiB kept for reuse\n",
                peak / 1048576.0, bound / 1048576.0, most_in_flight, (unsigned long long)fresh,
                HostBlockPool::keptBytes() / 1048576.0);
    if (peak > bound)
      fail("command memory above the bound");
    if (fresh)
      fail("blocks were allocated again instead of reused");
    m.commit(); // the chunk being recorded too
    while (!m.committed.empty())
      m.finishOne();
    recycle(~0ull);
  }
  idle.clear();
  HostBlockPool::releaseKept();
  if (ledger.snapshot().bytes[unsigned(guest_memory::Use::Commands)] != commands_before)
    fail("command blocks left after every list went");

  /* a block that can't be had: no memory and the failure counted (FinishCommandList returns
     E_OUTOFMEMORY from it); with memory again the ring goes on */
  {
    ListRing ring{HostBufferBlockAllocator{}};
    const uint64_t failures_before = guest_memory::ThreadFailures();
    g_fail_alloc = true;
    auto first = ring.allocate(1, 0, 4096, 16);
    const bool got = first.first.ptr != nullptr;
    g_fail_alloc = false;
    if (got)
      fail("a block without memory has memory");
    if (guest_memory::ThreadFailures() != failures_before + 1)
      fail("the failure wasn't counted");
    auto again = ring.allocate(1, 0, 4096, 16);
    if (!again.first.ptr)
      fail("the next block, with memory again, failed");
    ring.free_blocks(~0ull);
  }
  HostBlockPool::releaseKept();
  g_host_blocks_in_guest = was_guest;
  return failures;
}

/* Spec 06 round 4 (item 4): a deferred list's blocks start at 64 KB in a 32-bit process and
   double to 2 MB; 2,000 lists executed with no Present stay small because the backlog commits;
   past the command-memory mark FinishCommandList waits for the oldest committed chunk. */
int
CheckDeferredListBacklog() {
  int failures = 0;
  auto fail = [&](const char *what) {
    std::printf("FAIL deferred list backlog: %s\n", what);
    failures++;
  };
  constexpr uint64_t kKB = 1024, kMB = 1ull << 20;
  using ListRing = RingBumpState<HostBufferBlockAllocator, kStagingBlockSizeForDeferredContext, dxmt::null_mutex>;
  const bool was_guest = g_host_blocks_in_guest;
  g_host_blocks_in_guest = true;
  HostBlockPool::releaseKept();
  guest_memory::Ledger &ledger = guest_memory::ProcessLedger();
  auto commands = [&]() { return ledger.bytesOf(guest_memory::Use::Commands); };
  const uint64_t commands_before = commands();

  // the sizes: 64 KB first in 32-bit, doubling to 2 MB; 64-bit keeps 2 MB
  if (DeferredFirstBlockSize(true) != 64 * kKB || DeferredFirstBlockSize(false) != 2 * kMB)
    fail("the first block sizes");
  {
    ListRing ring{HostBufferBlockAllocator{}};
    ring.growFrom(DeferredFirstBlockSize(true));
    ring.allocate(1, 0, 100, 16);
    if (ring.blockBytes() != 64 * kKB)
      fail("a small list holds more than 64 KB");
    const uint64_t expected[] = {64, 128, 256, 512, 1024, 2048, 2048};
    uint64_t total = 0;
    bool doubled = true;
    for (uint64_t want : expected) {
      total += want * kKB;
      // fill the latest block, then one byte more makes the next
      while (ring.blockBytes() < total)
        ring.allocate(1, 0, 4 * kKB, 16);
      doubled &= ring.blockBytes() == total;
    }
    if (!doubled)
      fail("the blocks don't double from 64 KB to 2 MB");
    ring.free_blocks(~0ull);
    ring.restartGrowth();
    // a request larger than the next block gets a block of its own size; the growth goes on
    ring.allocate(1, 0, 16, 16);                    // 64 KB
    ring.allocate(1, 0, 3 * kMB, 16);               // its own 3 MB block
    if (ring.blockBytes() != 64 * kKB + 3 * kMB || ring.blockCount() != 2)
      fail("a large request didn't get its own block");
    if (ring.nextBlockSize() != 256 * kKB)
      fail("the growth stopped after a large request");
    ring.free_blocks(~0ull);
    ListRing fixed{HostBufferBlockAllocator{}};
    fixed.growFrom(DeferredFirstBlockSize(false));
    fixed.allocate(1, 0, 100, 16);
    if (fixed.blockBytes() != 2 * kMB)
      fail("a 64-bit list's first block isn't 2 MB");
    fixed.free_blocks(~0ull);
  }

  struct List {
    ListRing ring{HostBufferBlockAllocator{}};
    uint64_t seq = 0;
    List() { ring.growFrom(DeferredFirstBlockSize(true)); }
  };
  std::vector<std::unique_ptr<List>> idle;
  std::deque<std::unique_ptr<List>> in_flight;
  auto recycle = [&](uint64_t coherent) { // a released list goes back once its chunk has finished
    while (!in_flight.empty() && in_flight.front()->seq <= coherent) {
      in_flight.front()->ring.free_blocks(~0ull);
      in_flight.front()->ring.restartGrowth();
      idle.push_back(std::move(in_flight.front()));
      in_flight.pop_front();
    }
  };
  auto record = [&](List &list, size_t bytes, unsigned salt) {
    for (size_t recorded = 0; recorded < bytes;) {
      size_t n = 256 + (recorded * 7919u + salt) % 4096; // commands of 0.25-4.25 KB
      auto [block, offset] = list.ring.allocate(1, 0, n, 16);
      if (!block.ptr)
        return false;
      std::memset(static_cast<char *>(block.ptr) + offset, int(salt), n);
      recorded += n;
    }
    return true;
  };

  // 2,000 lists of 60-250 KB, finished and executed before any Present: the backlog commits
  {
    Model m(true);
    m.gpu_lag = 3; // the GPU finishes a chunk three commits after it
    CommandListBacklog backlog;
    uint64_t peak = 0, made = 0, backlog_commits = 0;
    std::string first_line;
    for (unsigned i = 0; i < 2000; i++) {
      recycle(m.coherent_seq);
      std::unique_ptr<List> list;
      if (idle.empty()) {
        list = std::make_unique<List>();
        made++;
      } else {
        list = std::move(idle.back());
        idle.pop_back();
      }
      if (!record(*list, 60 * kKB + (i * 37 % 190) * kKB, i)) {
        fail("a command got no memory");
        break;
      }
      // ExecuteCommandList: the chunk holds it; past the backlog, commit as a flush does
      const uint64_t bytes = list->ring.blockBytes();
      list->seq = m.current_seq;
      in_flight.push_back(std::move(list));
      if (backlog.executed(bytes)) {
        std::string line = backlog.takeFirstLine();
        if (!line.empty())
          first_line = line;
        backlog_commits++;
        m.commit();
        backlog.committed();
      }
      peak = std::max(peak, commands() - commands_before);
    }
    std::printf("deferred list backlog: 2000 lists with no Present: peak %.1f MiB of command memory, %llu lists "
                "made, %llu backlog commits; first line: %s\n",
                peak / 1048576.0, (unsigned long long)made, (unsigned long long)backlog_commits, first_line.c_str());
    if (peak > 200 * kMB)
      fail("command memory above 200 MB");
    if (made > 100)
      fail("lists weren't recycled");
    if (backlog_commits < 2000 / (kBacklogLists + 1))
      fail("too few backlog commits");
    if (first_line.rfind("[d3d11] command list backlog: 9 lists, ", 0) != 0 ||
        first_line.find(" MB since the last commit; committing") == std::string::npos)
      fail("the first backlog line");
    m.commit();
    while (!m.committed.empty())
      m.finishOne();
    recycle(~0ull);
    if (!in_flight.empty())
      fail("lists left in chunks");
  }

  // the wait: past the mark (here 32 MB for 1 GB), FinishCommandList waits for the oldest
  // committed chunk, whose lists then come back
  {
    constexpr uint64_t kMark = 32 * kMB;
    Model m(true);
    m.gpu_lag = 1u << 30; // the GPU finishes only what is waited for
    CommandListBacklog backlog;
    uint64_t peak = 0, waits = 0;
    for (unsigned i = 0; i < 600; i++) {
      recycle(m.coherent_seq);
      std::unique_ptr<List> list;
      if (idle.empty()) {
        list = std::make_unique<List>();
      } else {
        list = std::move(idle.back());
        idle.pop_back();
      }
      if (!record(*list, 900 * kKB, i)) {
        fail("a command got no memory (the wait)");
        break;
      }
      // FinishCommandList: past the mark, wait for the oldest committed chunk
      if (CommandMemoryWaitDue(true, commands() - commands_before, kMark)) {
        if (uint64_t oldest = OldestUnfinishedChunk(m.coherent_seq, m.current_seq)) {
          m.waitFor(oldest);
          waits++;
        }
      }
      const uint64_t bytes = list->ring.blockBytes();
      list->seq = m.current_seq;
      in_flight.push_back(std::move(list));
      if (backlog.executed(bytes)) {
        m.commit();
        backlog.committed();
      }
      peak = std::max(peak, commands() - commands_before);
    }
    std::printf("deferred list backlog: the wait at a %.0f MiB mark: peak %.1f MiB, %llu waits\n", kMark / 1048576.0,
                peak / 1048576.0, (unsigned long long)waits);
    if (!waits)
      fail("no wait at the mark");
    // the mark, one backlog's worth (16 MB plus a list), the free list (16 MB)
    if (peak > kMark + kBacklogBytes + 2 * kMB + kHostBlockKeepBytes)
      fail("command memory ran past the mark");
    m.commit();
    while (!m.committed.empty())
      m.finishOne();
    recycle(~0ull);
  }

  idle.clear();
  HostBlockPool::releaseKept();
  if (commands() != commands_before)
    fail("command blocks left after every list went");
  g_host_blocks_in_guest = was_guest;
  return failures;
}

int
main(int argc, char **argv) {
  bool check = false;
  int arg = 1;
  if (arg < argc && std::string(argv[arg]) == "--check") {
    check = true;
    arg++;
  }
  unsigned frames = check ? 4 : 10;
  unsigned cycles = 33000;
  bool new_mode = true;
  if (arg < argc)
    frames = unsigned(std::atoi(argv[arg++]));
  if (arg < argc)
    cycles = unsigned(std::atoi(argv[arg++]));
  if (arg < argc)
    new_mode = std::string(argv[arg++]) != "old";
  g_metal.alias = !new_mode;

  constexpr uint32_t kByteWidth = 64 * 1024;
  Rc<Buffer> buffer = new Buffer(kByteWidth, WMT::Device{});
  Flags<BufferAllocationFlag> flags;
  flags.set(BufferAllocationFlag::CpuWriteCombined);
  flags.set(BufferAllocationFlag::SuballocateFromOnePage);
  auto initial = buffer->rename(buffer->allocate(flags));
  (void)initial;
  Rc<DynamicBuffer> dyn = new DynamicBuffer(buffer.ptr(), flags);

  std::vector<uint8_t> source(kByteWidth, 0x5a);
  Model model(new_mode);

  // Two passes: with the 4-64 KB memcpy the game does, and without it (the
  // cost of map + unmap + bind + draw recording alone).
  double ns_full = 0, ns_map_only = 0;
  uint64_t new_buffers_after_first_frame = 0;
  for (int pass = 0; pass < 2; pass++) {
    bool copy = pass == 0;
    uint64_t tag = (uint64_t(pass) << 40) + 1;
    auto t0 = std::chrono::steady_clock::now();
    for (unsigned f = 0; f < frames; f++) {
      if (pass == 0 && f == 1)
        new_buffers_after_first_frame = g_metal.new_buffers;
      for (unsigned i = 0; i < cycles; i++, tag++) {
        auto *p = static_cast<uint8_t *>(model.mapDiscard(*dyn));
        size_t n = 4096 + (size_t(i) * 7919u) % (kByteWidth - 4096 + 1); // 4..64 KB
        if (copy)
          std::memcpy(p, source.data(), n);
        std::memcpy(p, &tag, sizeof(tag)); // what the draw will check it reads
        // Unmap: nothing; bind: the same buffer stays bound
        model.draw(*buffer, tag);
      }
      model.present();
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double(frames) * cycles);
    (copy ? ns_full : ns_map_only) = ns;
  }
  model.waitFor(model.current_seq); // drain

  uint64_t steady_new_buffers = g_metal.new_buffers - new_buffers_after_first_frame;
  std::printf(
      "bench_dynamic_discard (%s): %u frames x %u cycles of a 64 KB discard\n"
      "  per cycle: %.0f ns with the 4-64 KB memcpy, %.0f ns map+unmap+bind+draw alone\n"
      "  Metal buffers created: %llu (%llu after the first frame of the timed pass and later)\n"
      "  peak buffer memory held: %.1f MiB\n"
      "  commits: %llu (%llu presents), waits for the GPU: %llu, bad slice reads: %llu\n",
      new_mode ? "new" : "old", frames, cycles, ns_full, ns_map_only, (unsigned long long)g_metal.new_buffers,
      (unsigned long long)steady_new_buffers, g_metal.peak_bytes / 1048576.0, (unsigned long long)model.commits,
      (unsigned long long)(2 * frames), (unsigned long long)model.waits, (unsigned long long)model.bad_reads
  );

  std::printf("  [dxmt] implicit-commit log lines: %llu\n", (unsigned long long)model.logged_commits);

  if (!check)
    return 0;
  int failures = 0;
  // The log rule: the first implicit commit, then every 600th.
  const bool log_rule = DynamicDiscardTracker::logImplicitCommit(1) && !DynamicDiscardTracker::logImplicitCommit(2) &&
                        !DynamicDiscardTracker::logImplicitCommit(599) &&
                        DynamicDiscardTracker::logImplicitCommit(600) &&
                        !DynamicDiscardTracker::logImplicitCommit(601) &&
                        DynamicDiscardTracker::logImplicitCommit(1200) && !DynamicDiscardTracker::logImplicitCommit(0);
  if (!log_rule) {
    std::printf("FAIL: implicit-commit log rule\n");
    failures++;
  }
  if (new_mode && model.logged_commits == 0) {
    std::printf("FAIL: no implicit-commit log line\n");
    failures++;
  }
  if (model.bad_reads) {
    std::printf("FAIL: a draw read a slice that was reused before its chunk finished\n");
    failures++;
  }
  // Bounded: the in-flight cap, one commit's worth, the finished pool (at most
  // 64 allocations of at most 1 MiB) and the current allocation.
  const uint64_t bound = kDefaultDynamicInflightBytes + kDefaultDynamicCommitBytes + 65 * kDynamicAllocationTargetBytes;
  if (new_mode && g_metal.peak_bytes > bound) {
    std::printf("FAIL: peak %.1f MiB above the bound %.1f MiB\n", g_metal.peak_bytes / 1048576.0, bound / 1048576.0);
    failures++;
  }
  // After warming up, discards reuse memory: fewer new Metal buffers than one per 1000 cycles.
  if (new_mode && steady_new_buffers * 1000 > uint64_t(frames - 1 + frames) * cycles) {
    std::printf("FAIL: %llu Metal buffers created after the first frame\n", (unsigned long long)steady_new_buffers);
    failures++;
  }
  if (new_mode)
    failures += CheckDeferredRing(); // spec 05 clarification 67
  if (new_mode)
    failures += CheckGuestWindow(); // spec 06 round 2
  if (new_mode)
    failures += CheckCommandBlocks(); // spec 06 round 3 (item 2)
  if (new_mode)
    failures += CheckDeferredListBacklog(); // spec 06 round 4 (item 4)
  std::printf("bench_dynamic_discard --check: %d failures\n", failures);
  return failures ? 1 : 0;
}
