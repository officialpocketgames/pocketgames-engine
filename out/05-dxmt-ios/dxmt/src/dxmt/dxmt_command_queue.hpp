#pragma once
// Changed for MYIOSDECK (spec 05 E2, H1, H2, H5; spec 07 A2: LGPL-2.1-or-later blocks from
// dxmt-d3d9 v0.4-d3d9 (e8dd4c6), marked inline, see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT.

#include "Metal.hpp"
#include "dxmt_allocation.hpp"
#include "dxmt_capture.hpp"
#include "dxmt_command.hpp"
#include "dxmt_command_list.hpp"
#include "dxmt_context.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_statistics.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_cpu_fence.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <span>

namespace dxmt {

template <typename T> class moveonly_list {
public:
  moveonly_list(T *storage, size_t size) : storage(storage), size_(size) {
    for (unsigned i = 0; i < size_; i++) {
      new (storage + i) T();
    }
  }

  moveonly_list(const moveonly_list &copy) = delete;
  moveonly_list(moveonly_list &&move) {
    storage = move.storage;
    move.storage = nullptr;
    size_ = move.size_;
    move.size_ = 0;
  };

  ~moveonly_list() {
    for (unsigned i = 0; i < size_; i++) {
      storage[i].~T();
    }
  };

  T &
  operator[](int index) {
    return storage[index];
  }

  std::span<T>
  span() const {
    return std::span<T>(storage, size_);
  }

  T *
  data() const {
    return storage;
  }

  size_t
  size() const {
    return size_;
  }

private:
  T *storage;
  size_t size_;
};

constexpr uint32_t kCommandChunkCount = 32;

/* ---- spec 07: LGPL-2.1-or-later block, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6)
   dxmt_command_queue.hpp: GPU completion targets ---- */
enum class GpuCompletionStatus : uint8_t {
  Pending,
  Complete,
  Failed,
};

class GpuCompletionTarget {
public:
  GpuCompletionTarget() = default;
  GpuCompletionTarget(const GpuCompletionTarget &) = delete;
  GpuCompletionTarget &operator=(const GpuCompletionTarget &) = delete;
  virtual ~GpuCompletionTarget() noexcept = default;
  virtual void CompleteGpuWork(GpuCompletionStatus status) noexcept = 0;
};
/* ---- end of LGPL block ---- */

class CommandQueue;

class CommandChunk {
public:
  CommandChunk(const CommandChunk &) = delete; // delete copy constructor

  void *
  allocate_cpu_heap(size_t size, size_t alignment);

  template <CommandWithContext<ArgumentEncodingContext> F>
  void
  emitcc(F &&func) {
    list_enc.emit(std::forward<F>(func), allocate_cpu_heap(list_enc.calculateCommandSize<F>(), 16));
  }

  /* MYIOSDECK spec 05 E2.3 */
  bool
  hasCommands() const {
    return list_enc.hasCommands();
  }

  void
  encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc) {
    enc.$$setEncodingContext(
      chunk_id,
      frame_
    );
    auto& statistics = enc.currentFrameStatistics();
    auto t0 = clock::now();
    list_enc.execute(enc);
    attached_cmdbuf = cmdbuf;
    auto t1 = clock::now();
    visibility_readback = enc.flushCommands(cmdbuf, chunk_id, chunk_event_id);
    auto t2 = clock::now();
    statistics.encode_prepare_interval += (t1 - t0);
    statistics.encode_flush_interval += (t2 - t1);
  };

  /* spec 07: LGPL-2.1-or-later, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6). Notified
     by the finish thread when this chunk's command buffer has completed (or
     failed). */
  void
  addCompletionTarget(std::shared_ptr<GpuCompletionTarget> target) {
    if (!target)
      return;
    std::lock_guard<dxmt::mutex> lock(completion_targets_mutex_);
    completion_targets.push_back(std::move(target));
  }

  uint64_t chunk_id;
  uint64_t chunk_event_id;
  uint64_t frame_;
  uint64_t signal_frame_latency_fence_;
  std::unique_ptr<VisibilityResultReadback> visibility_readback;
  uint64_t resource_initializer_event_id;
  std::vector<std::shared_ptr<GpuCompletionTarget>> completion_targets; /* spec 07 */
  dxmt::mutex completion_targets_mutex_;                                 /* spec 07 */

private:
  CommandQueue *queue;
  WMT::Reference<WMT::CommandBuffer> attached_cmdbuf;

  CommandList<ArgumentEncodingContext> list_enc;
  AllocationRefTracking ref_tracker;

  friend class CommandQueue;

public:
  CommandChunk() {}

  void
  reset() noexcept {
    signal_frame_latency_fence_ = ~0ull;
    visibility_readback = {};
    completion_targets.clear(); /* spec 07 */
    list_enc.reset();
    ref_tracker.clear();
    attached_cmdbuf = nullptr;
  }
};

class CommandQueue {

private:
  void CommitChunkInternal(CommandChunk &chunk, uint64_t seq);

#ifdef DXMT_IOS
  /* spec 05 H5 (finish thread only) */
  void AccountGpuBusyTime(CommandChunk &chunk);
  bool gpu_perf_window_started_ = false;
  uint64_t gpu_perf_window_frame_ = 0;
  std::chrono::steady_clock::time_point gpu_perf_window_start_;
  double gpu_perf_busy_ = 0;
  double gpu_perf_prev_end_ = 0;
#endif

  uint32_t EncodingThread();

  uint32_t WaitForFinishThread();

  std::atomic_uint64_t ready_for_encode = 1; // we start from 1, so 0 is always coherent
  std::atomic_uint64_t ready_for_commit = 1;
  std::atomic_uint64_t chunk_ongoing = 0;
  CpuFence cpu_coherent;
  CpuFence frame_latency_fence_;
  std::atomic_bool stopped;
  std::atomic<bool> device_error_ = false; /* spec 07 (LGPL, from v0.4-d3d9) */

  std::array<CommandChunk, kCommandChunkCount> chunks;
  uint64_t encoder_seq = 1;
  uint64_t frame_count = 0;
  uint32_t max_latency_ = 3;

  dxmt::thread encodeThread;
  dxmt::thread finishThread;
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> commandQueue;

  obj_handle_t shared_event_listener;
  dxmt::thread event_listener_thread;

  friend class CommandChunk;
  uint64_t
  GetNextEncoderId() {
    return encoder_seq++;
  }

  RingBumpState<StagingBufferBlockAllocator> staging_allocator;
  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator;
  RingBumpState<StagingBufferBlockAllocator, kCommandChunkGPUHeapSize> argbuf_allocator;
  RingBumpState<HostBufferBlockAllocator, kCommandChunkCPUHeapSize, dxmt::null_mutex> cpu_command_allocator;
  RingBumpState<HostBufferBlockAllocator, 0x1000 /* 4kB */> reftracker_storage_allocator;
  CaptureState capture_state;

public:
  InternalCommandLibrary cmd_library;
  ArgumentEncodingContext argument_encoding_ctx;
  WMT::Reference<WMT::SharedEvent> event;
  std::uint64_t current_event_seq_id = 0;
  FrameStatisticsContainer statistics;
  ResourceInitializer initializer;

  CommandQueue(WMT::Device device);

  ~CommandQueue();

  CommandChunk *
  CurrentChunk() {
    auto id = ready_for_encode.load(std::memory_order_relaxed);
    return &chunks[id % kCommandChunkCount];
  };

  uint64_t
  CoherentSeqId() {
    return cpu_coherent.signaledValue();
  };

  uint64_t
  CurrentSeqId() {
    return ready_for_encode.load(std::memory_order_relaxed);
  };

  uint64_t
  GetNextEventSeqId() {
    return ++current_event_seq_id;
  };

  uint64_t
  GetCurrentEventSeqId() {
    return current_event_seq_id;
  };


  uint64_t
  SignaledEventSeqId() {
    return event.signaledValue();
  };

  obj_handle_t GetSharedEventListener() {
    return shared_event_listener;
  }

  /**
  This is not thread-safe!
  CurrentChunk & CommitCurrentChunk should be called on the same thread

  */
  void CommitCurrentChunk();

  uint64_t CurrentFrameSeq() {
    return frame_count + 1;
  }

  FrameStatistics& CurrentFrameStatistics() {
    return statistics.at(frame_count);
  }

  void
  PresentBoundary() {
    statistics.compute(frame_count);
    frame_count++;
    statistics.at(frame_count).reset();
    // After present N-th frame (N starts from 1), wait for (N - max_latency)-th frame to finish rendering 
    if (likely(frame_count > max_latency_)) {
      auto t0 = clock::now();
      frame_latency_fence_.wait(frame_count - max_latency_);
      auto t1 = clock::now();
      statistics.at(frame_count).present_lantency_interval += (t1 - t0);
    }
    statistics.at(frame_count).latency = max_latency_;
  }

  uint32_t GetMaxLatency() { return max_latency_; }

  void SetMaxLatency(uint32_t value) { max_latency_ = value; };

  /* ---- spec 07: LGPL-2.1-or-later block, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6)
     dxmt_command_queue.hpp ---- */
  bool HasDeviceError() const {
    return device_error_.load(std::memory_order_acquire);
  }

  void MarkDeviceError();

  uint64_t FrameLatencySignaled() { return frame_latency_fence_.signaledValue(); }

  void WaitFrameLatency(uint64_t frame_seq) {
    if (frame_seq > max_latency_)
      frame_latency_fence_.wait(frame_seq - max_latency_);
  }
  /* ---- end of LGPL block ---- */

  void
  WaitCPUFence(uint64_t seq) {
    cpu_coherent.wait(seq);
  };

#ifdef DXMT_IOS
  /**
  Spec 05 H2: true at once if the CPU-coherent sequence has reached `seq`;
  otherwise spin 64 reloads, then yield the thread until the deadline,
  re-checking after each yield. Returns whether `seq` was reached; never
  waits longer than `timeout_ns`. (Spec 07's D3D9 queries use it.)
  */
  bool
  WaitCPUFenceBounded(uint64_t seq, uint64_t timeout_ns) {
    if (cpu_coherent.signaledValue() >= seq)
      return true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
    for (unsigned i = 0; i < 64; i++) {
      if (cpu_coherent.signaledValue() >= seq)
        return true;
    }
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
      if (cpu_coherent.signaledValue() >= seq)
        return true;
    }
    return cpu_coherent.signaledValue() >= seq;
  }
#endif

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset, block.gpu_address};
  }

  std::tuple<void *, WMT::Buffer, uint64_t>
  AllocateArgumentBuffer(uint64_t seq, size_t size) {
    auto [block, offset] = argbuf_allocator.allocate(seq, cpu_coherent.signaledValue(), size, 64);
    return {ptr_add(block.mapped_address, offset), block.buffer, offset};
  }

  void *
  AllocateCommandData(size_t size, size_t alignment) {
    auto [block, offset] =
        cpu_command_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return ptr_add(block.ptr, offset);
  }

  void Retain(uint64_t seq, Allocation *allocation);
};

} // namespace dxmt