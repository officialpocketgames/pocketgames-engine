// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 Part B: the platform layer of the ios_wow_native build (see util_ios_wow_native.h),
// and wsi::aligned_malloc / aligned_free for that build (B2, Part C).

#ifdef DXMT_IOS_WOW_NATIVE

#include "util_ios_wow_native.h"
#include "wsi_platform.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

extern "C" {
__thread uint32_t dxmt_native_thread_id = 0;
}

namespace dxmt::ios_wow_native {

namespace {

__thread int t_qos = 0;
std::mutex g_objects_mutex;
uint32_t g_generation = 1;
uint64_t g_live_current = 0; /* objects of g_generation */
uint64_t g_live_all = 0;     /* orphans included */
std::atomic<uint32_t> g_fallback_logs{0};
std::atomic<uint32_t> g_next_fallback_id{1};

constexpr size_t kMetalPage = 16384; /* iOS pages; spec 06 clarification 1 */

} // namespace

/* ---- threads ---- */

uint32_t
initThreadId() {
  uint32_t id = 0;
#if defined(__APPLE__)
  uint64_t tid = 0;
  if (pthread_threadid_np(nullptr, &tid) == 0)
    id = uint32_t(tid) ^ uint32_t(tid >> 32);
#elif defined(__linux__)
  id = uint32_t(syscall(SYS_gettid));
#endif
  /* 0 means "no owner" to the device lock: never hand it out. */
  if (!id)
    id = 0x80000000u | g_next_fallback_id.fetch_add(1, std::memory_order_relaxed);
  dxmt_native_thread_id = id;
  return id;
}

void
setCurrentThreadPriority(int windows_priority) {
  int qos = qosForPriority(windows_priority);
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(qos_class_t(qos), 0);
#endif
  t_qos = qos;
}

int
currentThreadQos() {
  return t_qos;
}

void
onNativeThreadStart() {
  ios_wow_window_mark_native_thread();
  /* the encode, finish and event threads and the PSO workers: user-interactive (B3) */
  setCurrentThreadPriority(THREAD_PRIORITY_TIME_CRITICAL);
}

bool
isNativeThread() {
  return ios_wow_window_is_native_thread() != 0;
}

void
wineThreadRequired(const char *what) {
  if (!isNativeThread())
    return;
  char line[160];
  std::snprintf(line, sizeof(line), "err:   d3d9 (native): %s on a native thread (spec 33 B3)\n", what);
  if (__wine_dbg_output)
    __wine_dbg_output(line);
  else
    std::fputs(line, stderr);
  std::abort();
}

/* ---- COM objects ---- */

/* Clarification 13: a generation per process that used the frontend. One mutex keeps the
   generation and its count consistent when objects are made or destroyed (on any thread) while a
   process ends; objects are made at resource creation, not per call. */
uint32_t
objectBorn() {
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  g_live_all++;
  g_live_current++;
  return g_generation;
}

void
objectGone(void *iface, uint32_t generation) {
  if (d3d9w_object_gone)
    d3d9w_object_gone(iface);
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  g_live_all--;
  if (generation == g_generation)
    g_live_current--;
}

uint64_t
liveObjects() {
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  return g_live_current;
}

uint64_t
allLiveObjects() {
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  return g_live_all;
}

uint64_t
orphanLiveObjects() {
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  const uint64_t orphaned = g_live_current;
  g_generation++;
  g_live_current = 0;
  return orphaned;
}

/* ---- memory the program may see ---- */

void *
allocVisible(size_t size, size_t alignment) {
  if (alignment < alignof(std::max_align_t))
    alignment = alignof(std::max_align_t);
  /* page alignment asked for: the memory may become a no-copy Metal buffer */
  if (alignment >= DXMT_PAGE_SIZE) {
    if (alignment < kMetalPage)
      alignment = kMetalPage;
    size = (size + kMetalPage - 1) & ~(kMetalPage - 1);
  }
  if (!isNativeThread()) {
    uint32_t guest = 0;
    if (void *p = ios_wow_window_alloc(size, alignment, &guest))
      return p;
  }
  /* A native thread, or no room in the window: the native heap. Such memory has no guest
     address, so it must never reach the program (Part C2); log the first few. */
  if (g_fallback_logs.fetch_add(1, std::memory_order_relaxed) < 8)
    Logger::warn(str::format(
        "d3d9 (native): ", size, " bytes from the native heap (",
        isNativeThread() ? "native thread" : "no guest window, or no room in it", ")"
    ));
  return allocHost(size, alignment);
}

void *
allocHost(size_t size, size_t alignment) {
  if (alignment < alignof(std::max_align_t))
    alignment = alignof(std::max_align_t);
  void *p = nullptr;
  if (posix_memalign(&p, alignment, size ? size : 1) != 0)
    return nullptr;
  return p;
}

void
freeVisible(void *ptr) {
  if (!ptr)
    return;
  if (ios_wow_window_owns(ptr))
    ios_wow_window_free(ptr);
  else
    std::free(ptr);
}

} // namespace dxmt::ios_wow_native

namespace dxmt::wsi {

void *
aligned_malloc(size_t size, size_t alignment) {
  return ios_wow_native::allocVisible(size, alignment);
}

void
aligned_free(void *ptr) {
  ios_wow_native::freeVisible(ptr);
}

} // namespace dxmt::wsi

#endif /* DXMT_IOS_WOW_NATIVE */
