// Changed for MYIOSDECK (spec 05 B9, D2, H2, H5; spec 06 D; spec 07 A2: LGPL-2.1-or-later blocks
// from dxmt-d3d9 v0.4-d3d9 (e8dd4c6), marked inline, see LICENSE-NOTES.md; spec 05 clarification 10:
// LGPL-2.1-or-later blocks from steamac's DXMT patches 0019 and 0023, marked inline). Upstream: DXMT
// a3a6e8f, MIT.
#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include "dxmt_chunk_slots.hpp"
#include "dxmt_statistics.hpp"
#include "util_atomic_wait.hpp"
#include "util_bitness.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>
#include <chrono>

#define ASYNC_ENCODING 1

namespace dxmt {

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

CommandQueue::CommandQueue(WMT::Device device) :
    encodeThread([this]() { this->EncodingThread(); }),
    finishThread([this]() { this->WaitForFinishThread(); }),
    device(device),
    commandQueue(device.newCommandQueue(kCommandChunkCount)),
    shared_event_listener(SharedEventListener_create()),
    event_listener_thread([this]() { SharedEventListener_start(this->shared_event_listener); }),
    staging_allocator({
        device, WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                    WMTResourceStorageModeManaged, false
    }),
    copy_temp_allocator({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
    argbuf_allocator({
        device,
        WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeWriteCombined | WMTResourceStorageModeShared
    }),
    cpu_command_allocator({}),
    reftracker_storage_allocator({}),
    cmd_library(device),
    argument_encoding_ctx(*this, device, cmd_library),
    initializer(device) {
  /* LGPL-2.1-or-later, from steamac's DXMT patch 0023 (Jakey-Jakey), fitted for MYIOSDECK
     spec 05 clarification 10: a residency set attached to the queue replaces per-encoder
     useResource. MTLResidencySet needs iOS 18 and the app supports iOS 17, so below 18 the
     old path stays; so does the i386 build (its unix calls are refused) and
     DXMT_RESIDENCY_SET=0. One log line says which path is in use. */
  {
    uint64_t os_major = 0, os_minor = 0, os_patch = 0;
    WMTGetOSVersion(&os_major, &os_minor, &os_patch);
    const char *without = nullptr;
    if (kI386Build) {
      without = "32-bit program";
    } else if (os_major < 18) {
      without = "iOS below 18";
    } else if (env::getEnvVar("DXMT_RESIDENCY_SET") == "0") {
      without = "DXMT_RESIDENCY_SET=0";
    } else {
      WMT::Reference<WMT::Error> err;
      auto set = device.newResidencySet(0, err);
      if (set != nullptr) {
        commandQueue.addResidencySet(set);
        residency_set = new ResidencySetTracker(std::move(set));
        argument_encoding_ctx.setResidencySet(residency_set.ptr());
      } else {
        WARN("Failed to create MTLResidencySet: ", err ? err.description().getUTF8String() : std::string("no error"));
        without = "MTLResidencySet creation failed";
      }
    }
    if (without)
      Logger::info(str::format("residency: useResource per encoder (", without, "; iOS ", os_major, ".", os_minor, ")"));
    else
      Logger::info(str::format("residency: MTLResidencySet (iOS ", os_major, ".", os_minor, ")"));
  }
  /* end of LGPL block */

  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.queue = this;
    chunk.reset();
  };
  event = device.newSharedEvent();

  std::string env = env::getEnvVar("DXMT_CAPTURE_FRAME");

  if (!env.empty()) {
    try {
      capture_state.scheduleNextFrameCapture(std::stoull(env));
    } catch (const std::invalid_argument &) {
    }
  }
}

CommandQueue::~CommandQueue() {
  TRACE("Destructing command queue");
  stopped.store(true);
  ready_for_encode++;
  atomic_notify_one(ready_for_encode);
  ready_for_commit++;
  atomic_notify_one(ready_for_commit);
  SharedEventListener_destroy(shared_event_listener);
  encodeThread.join();
  finishThread.join();
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.reset();
  };
  event_listener_thread.join();
  TRACE("Destructed command queue");
}

void
CommandQueue::CommitCurrentChunk() {
  auto& statistics = CurrentFrameStatistics();
#if ASYNC_ENCODING
  /* LGPL-2.1-or-later, from steamac's DXMT patch 0019 (时雨): reserve the chunk's slot
     before publishing it (dxmt_chunk_slots.hpp); upstream published first and counted
     the chunk in afterwards, so the finish thread could release it before that. */
  auto t0 = clock::now();
  ReserveChunkSlot(chunk_ongoing, kCommandChunkCount - 1);
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);
#endif

  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % kCommandChunkCount];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_count;
  chunk.resource_initializer_event_id = initializer.flushToWait();
  statistics.command_buffer_count++;
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  atomic_notify_one(ready_for_encode);
  /* end of LGPL block */
#else
  CommitChunkInternal(chunk, ready_for_encode.fetch_add(1, std::memory_order_relaxed));
#endif

  cpu_command_allocator.free_blocks(cpu_coherent.signaledValue());
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {

  // Spec 05 B9: everything the unix-side thunks autorelease while this chunk is
  // encoded (above all the frame's CAMetalDrawable) goes into this pool, which
  // is drained when it is destroyed as the last local of this function. The
  // command buffer retains the drawable until it is presented.
  auto pool = WMT::MakeAutoreleasePool();

  switch (capture_state.getNextAction(chunk.frame_)) {
  case CaptureState::NextAction::StartCapture: {
    WMTCaptureInfo info;
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    auto fileUrl = env::getUnixPath(env::getExeBaseName() + "_F." + std::to_string(chunk.frame_) + filename);
    WARN("A new capture will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());

    capture_mgr.startCapture(info);
    break;
  }
  case CaptureState::NextAction::StopCapture: {
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    capture_mgr.stopCapture();
    break;
  }
  case CaptureState::NextAction::Nothing: {
    if (capture_state.shouldCaptureNextFrame()) {
      capture_state.scheduleNextFrameCapture(chunk.frame_ + 1);
    }
    break;
  }
  }

  auto cmdbuf = commandQueue.commandBuffer();
  chunk.attached_cmdbuf = cmdbuf;
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
  }
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
  if (residency_set.ptr()) /* patch 0023 (LGPL): what this chunk added is resident before it runs */
    residency_set->commit();
  cmdbuf.commit();

  ready_for_commit.fetch_add(1, std::memory_order_release);
  atomic_notify_one(ready_for_commit);
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("dxmt-encode-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    atomic_wait(ready_for_encode, internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    // perform...
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    CommitChunkInternal(chunk, internal_seq);
    internal_seq++;
  }
  TRACE("encoder thread gracefully terminates");
#endif
  return 0;
}

uint32_t
CommandQueue::WaitForFinishThread() {
  env::setThreadName("dxmt-finish-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    atomic_wait(ready_for_commit, internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    if (chunk.attached_cmdbuf.status() <= WMTCommandBufferStatusScheduled) {
      chunk.attached_cmdbuf.waitUntilCompleted();
    }
    /* spec 07 (LGPL, from v0.4-d3d9): device-error tracking */
    const bool device_error = chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError;
    if (device_error) {
      MarkDeviceError();
      ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
    }
    if (auto logs = chunk.attached_cmdbuf.logs()) {
      for (auto &log : logs.elements()) {
        ERR("Frame ", chunk.frame_, ": ", log.description().getUTF8String());
      }
    }
#ifdef DXMT_IOS
    AccountGpuBusyTime(chunk);
#endif

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    /* spec 07 (LGPL, from v0.4-d3d9): GPU completion targets */
    {
      std::lock_guard<dxmt::mutex> lock(chunk.completion_targets_mutex_);
      for (const auto &target : chunk.completion_targets)
        target->CompleteGpuWork(device_error ? GpuCompletionStatus::Failed : GpuCompletionStatus::Complete);
    }

    chunk.reset();
    cpu_coherent.signal(internal_seq);
    ReleaseChunkSlot(chunk_ongoing); /* patch 0019 (LGPL): the release half, dxmt_chunk_slots.hpp */

    staging_allocator.free_blocks(internal_seq);
    copy_temp_allocator.free_blocks(internal_seq);
    argbuf_allocator.free_blocks(internal_seq);

    internal_seq++;
  }
  TRACE("finishing thread gracefully terminates");
  return 0;
}

#ifdef DXMT_IOS
/**
Spec 05 H5 (DXMT_GPU_PERF=1, off by default): for each retired command
buffer, take its GPU start and end times from the unix half (control call op
3) and add the busy time (end - max(start, previous end), when positive). One
log line per 64 frames: frames, wall time, fps, busy time and percentage.
Runs on the finish thread only.
*/
void
CommandQueue::AccountGpuBusyTime(CommandChunk &chunk) {
  static const bool enabled = env::getEnvVar("DXMT_GPU_PERF") == "1";
  if (!enabled)
    return;
  double start = 0, end = 0;
  if (MTLCommandBuffer_getGPUTimes(chunk.attached_cmdbuf.handle, &start, &end)) {
    double from = std::max(start, gpu_perf_prev_end_);
    if (end - from > 0)
      gpu_perf_busy_ += end - from;
    gpu_perf_prev_end_ = std::max(gpu_perf_prev_end_, end);
  }
  auto now = std::chrono::steady_clock::now();
  if (!gpu_perf_window_started_) {
    gpu_perf_window_started_ = true;
    gpu_perf_window_frame_ = chunk.frame_;
    gpu_perf_window_start_ = now;
    return;
  }
  uint64_t frames = chunk.frame_ - gpu_perf_window_frame_;
  if (frames < 64)
    return;
  double wall = std::chrono::duration<double>(now - gpu_perf_window_start_).count();
  double fps = wall > 0 ? frames / wall : 0;
  double percent = wall > 0 ? gpu_perf_busy_ / wall * 100.0 : 0;
  Logger::info(str::format(
      "GPU perf: ", frames, " frames in ", wall * 1000.0, " ms (", fps, " fps), GPU busy ", gpu_perf_busy_ * 1000.0,
      " ms (", percent, "%)"
  ));
  gpu_perf_window_frame_ = chunk.frame_;
  gpu_perf_window_start_ = now;
  gpu_perf_busy_ = 0;
}
#endif

void CommandQueue::Retain(uint64_t seq, Allocation* allocaiton) {
  auto &chunk = chunks[seq % kCommandChunkCount];
  auto &tracker = chunk.ref_tracker;
  constexpr size_t block_size = decltype(reftracker_storage_allocator)::block_size;
  while (unlikely(!tracker.track(allocaiton))) {
    auto [temp_buffer, _] = reftracker_storage_allocator.allocate(seq, cpu_coherent.signaledValue(), block_size, 1);
    tracker.addStorage(temp_buffer.ptr, block_size);
  }
};

/* spec 07: LGPL-2.1-or-later, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6) */
void
CommandQueue::MarkDeviceError() {
  device_error_.store(true, std::memory_order_release);
}

} // namespace dxmt
