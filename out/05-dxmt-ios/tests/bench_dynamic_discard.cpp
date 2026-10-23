// SPDX-License-Identifier: MIT
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

#include "dxmt_buffer.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_dynamic_policy.hpp"
#include "dxmt_format.hpp"
#include "wsi_platform.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
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

namespace wsi {
void *
aligned_malloc(size_t size, size_t alignment) {
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
      dyn.updateImmediateName(current_seq, dyn.allocate(coherent_seq, nullptr, new_mode ? current_seq : 0), 0, false);
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

} // namespace

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
  std::printf("bench_dynamic_discard --check: %d failures\n", failures);
  return failures ? 1 : 0;
}
