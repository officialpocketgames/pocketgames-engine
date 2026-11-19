// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 Part J5: the ios_wow_native platform layer on the Linux host (util_ios_wow_native.cpp
// with Part C's allocator and fakes of Wine's memory calls; no Wine tree needed):
//   - B3 threads: dxmt::thread starts are marked native and run at QoS user-interactive;
//     SetThreadPriority(GetCurrentThread(), ...) maps priorities to QoS without logging;
//     GetCurrentThreadId is per thread, non-zero and stable;
//   - B3 locks: the device lock (D9RecursiveSpinlock over GetCurrentThreadId, YieldProcessor and
//     SwitchToThread) is recursive and exclusive under contention; dxmt::mutex and
//     condition_variable are the std ones;
//   - B3 address wait: dxmt::atomic_wait / notify (std::atomic's, clarification 9) across the
//     frontend's threads;
//   - Part C routing: wsi::aligned_malloc is guest-window memory on a game thread (16 KB pages
//     for page-aligned requests), native heap on a native thread or a thread without a window;
//     allocHost is never in the window; aligned_free takes both;
//   - B4 wrappers: the user32-named wrappers convert to and from util_ios_wow_native_user.h;
//   - clarification 7: ComObject reports every object destroyed to d3d9w_object_gone once,
//     with its first interface; the focus window actions (d3d9_native_focus.hpp);
//   - the allocator's two declarations (alloc/ios_wow_window_alloc.h and this layer's) agree.

#include "ios_wow_window_alloc.h" /* both declarations in one translation unit: they must agree */

#include "com/com_object.hpp"
#include "d3d9_multithread.hpp"
#include "d3d9_native_focus.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_atomic_wait.hpp"
#include "util_win32_compat.h"
#include "wsi_platform.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <vector>

namespace dxmt {
Logger Logger::s_instance("test_ios_wow_native_platform.log");
}

static int failures, checks;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(cond)) {                                                                                                     \
      failures++;                                                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                    \
    }                                                                                                                  \
  } while (0)

/* ---- fakes: Wine's memory calls and ios_wow_base over one 4 GB-aligned window ---- */

static uintptr_t g_window, g_next;
static thread_local bool t_no_window;

extern "C" {
unsigned long
ios_wow_base(void) {
  return t_no_window ? 0 : g_window;
}
int32_t
NtAllocateVirtualMemory(void *, void **ret, uintptr_t, size_t *size_ptr, uint32_t, uint32_t) {
  static std::atomic_flag busy = ATOMIC_FLAG_INIT;
  while (busy.test_and_set())
    ;
  void *p = mmap(reinterpret_cast<void *>(g_next), *size_ptr, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  g_next += *size_ptr;
  busy.clear();
  if (p == MAP_FAILED)
    return int32_t(0xc0000017);
  *ret = p;
  return 0;
}
int32_t
NtFreeVirtualMemory(void *, void **, size_t *, uint32_t) {
  return 0;
}
}

/* ---- fakes: the B4 shim, to check the wrappers' conversions ---- */

extern "C" {
int
dxmt_native_user_get_client_rect(void *hwnd, dxmt_native_rect *rect) {
  if (hwnd != reinterpret_cast<void *>(0x10020))
    return 0;
  *rect = {1, 2, 641, 482};
  return 1;
}
int
dxmt_native_user_client_to_screen(void *, int32_t *x, int32_t *y) {
  *x += 7;
  *y += 9;
  return 1;
}
int
dxmt_native_user_is_window(void *hwnd) {
  return hwnd == reinterpret_cast<void *>(0x10020);
}
int
dxmt_native_user_is_window_visible(void *) {
  return 0;
}
int
dxmt_native_user_get_system_metrics(int index) {
  return index == SM_CXSCREEN ? 1024 : 768;
}
void *
dxmt_native_user_monitor_from_window(void *, uint32_t flags) {
  return reinterpret_cast<void *>(uintptr_t(0x100 + flags));
}
void *
dxmt_native_user_monitor_from_point(int32_t x, int32_t y, uint32_t flags) {
  return reinterpret_cast<void *>(uintptr_t(0x200 + x + y + flags));
}
int
dxmt_native_user_get_monitor_info(void *, dxmt_native_rect *m, dxmt_native_rect *w, uint32_t *flags) {
  *m = {0, 0, 1024, 768};
  *w = {0, 0, 1024, 740};
  *flags = 1;
  return 1;
}
int
dxmt_native_user_enum_display_settings(uint32_t mode, dxmt_native_display_mode *out) {
  if (mode != ENUM_CURRENT_SETTINGS)
    return 0;
  *out = {1024, 768, 32, 60};
  return 1;
}
}

/* ---- the seam's object hook ---- */

static std::vector<void *> g_gone;
extern "C" void
d3d9w_object_gone(void *iface) {
  g_gone.push_back(iface);
}

/* a COM object with a second base, as the device has */
struct ISecond {
  virtual void second() = 0;
};
struct TestObject final : public dxmt::ComObject<IUnknown>, public ISecond {
  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID, void **out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  void
  second() override {}
};

static void
testThreads() {
  namespace native = dxmt::ios_wow_native;
  CHECK(!native::isNativeThread());
  const uint32_t main_id = GetCurrentThreadId();
  CHECK(main_id != 0 && GetCurrentThreadId() == main_id);

  std::atomic<bool> native_flag{false};
  std::atomic<int> start_qos{0}, critical_qos{0}, low_qos{0};
  std::atomic<uint32_t> ids[2] = {0, 0};
  dxmt::thread a([&]() {
    native_flag = native::isNativeThread();
    start_qos = native::currentThreadQos();
    ids[0] = GetCurrentThreadId();
    /* the encode, finish and PSO threads ask for this at their start */
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL));
    critical_qos = native::currentThreadQos();
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST));
    low_qos = native::currentThreadQos();
  });
  dxmt::thread b([&]() { ids[1] = GetCurrentThreadId(); });
  a.join();
  b.join();
  CHECK(native_flag.load());
  CHECK(start_qos.load() == native::kQosUserInteractive);
  CHECK(critical_qos.load() == native::kQosUserInteractive);
  CHECK(low_qos.load() == native::kQosUtility);
  CHECK(ids[0] && ids[1] && ids[0] != ids[1] && ids[0] != main_id && ids[1] != main_id);
  /* another thread's handle is not changed */
  CHECK(!SetThreadPriority(reinterpret_cast<HANDLE>(uintptr_t(0x1234)), THREAD_PRIORITY_NORMAL));
  CHECK(native::qosForPriority(THREAD_PRIORITY_HIGHEST) == native::kQosUserInteractive);
  CHECK(native::qosForPriority(THREAD_PRIORITY_BELOW_NORMAL) == native::kQosUtility);
  CHECK(native::qosForPriority(THREAD_PRIORITY_IDLE) == native::kQosBackground);

  /* moving a thread object keeps it joinable once */
  dxmt::thread c([]() {});
  dxmt::thread moved = std::move(c);
  CHECK(!c.joinable() && moved.joinable());
  moved.join();
  YieldProcessor();
  CHECK(SwitchToThread());
}

static void
testLocks() {
  dxmt::D9RecursiveSpinlock lock;
  lock.lock();
  lock.lock(); /* recursive */
  std::atomic<bool> got{true};
  dxmt::thread probe([&]() { got = lock.try_lock(); });
  probe.join();
  CHECK(!got.load());
  lock.unlock();
  lock.unlock();

  /* contention: four threads, each adding under the lock */
  constexpr int kThreads = 4, kRounds = 20000;
  int counter = 0;
  std::vector<dxmt::thread> workers;
  for (int i = 0; i < kThreads; i++)
    workers.emplace_back([&]() {
      for (int r = 0; r < kRounds; r++) {
        lock.lock();
        if (r % 1000 == 0) {
          lock.lock(); /* nested on the same thread */
          counter++;
          lock.unlock();
        } else {
          counter++;
        }
        lock.unlock();
      }
    });
  for (auto &w : workers)
    w.join();
  CHECK(counter == kThreads * kRounds);

  /* dxmt::mutex and condition_variable are std's */
  dxmt::mutex m;
  dxmt::condition_variable cv;
  bool ready = false;
  dxmt::thread signaller([&]() {
    std::lock_guard<dxmt::mutex> guard(m);
    ready = true;
    cv.notify_one();
  });
  {
    std::unique_lock<dxmt::mutex> guard(m);
    cv.wait(guard, [&]() { return ready; });
  }
  signaller.join();
  CHECK(ready);
}

static void
testAddressWait() {
  std::atomic<uint64_t> word{5};
  std::atomic<int> woken{0};
  std::vector<dxmt::thread> waiters;
  for (int i = 0; i < 3; i++)
    waiters.emplace_back([&]() {
      dxmt::atomic_wait(word, uint64_t(5));
      woken++;
    });
  for (int i = 0; i < 100; i++)
    SwitchToThread();
  CHECK(woken.load() == 0);
  word.store(6);
  dxmt::atomic_notify_all(word);
  for (auto &w : waiters)
    w.join();
  CHECK(woken.load() == 3);
  dxmt::atomic_wait(word, uint64_t(5)); /* already changed: returns at once */
  std::atomic<uint32_t> one{0};
  dxmt::thread single([&]() { dxmt::atomic_wait(one, 0u); });
  one.store(1);
  dxmt::atomic_notify_one(one);
  single.join();
}

static bool
inWindow(void *p) {
  return p && uintptr_t(p) - g_window < 0x100000000ull;
}

static void
testMemory() {
  namespace native = dxmt::ios_wow_native;
  void *mirror = dxmt::wsi::aligned_malloc(100, DXMT_PAGE_SIZE);
  CHECK(inWindow(mirror));
  CHECK((uintptr_t(mirror) & 16383) == 0);
  void *after = dxmt::wsi::aligned_malloc(100, DXMT_PAGE_SIZE);
  CHECK(inWindow(after) && uintptr_t(after) - uintptr_t(mirror) >= 16384);
  void *small = dxmt::wsi::aligned_malloc(24, 8);
  CHECK(inWindow(small) && (uintptr_t(small) & 15) == 0);
  CHECK(ios_wow_window_guest(small) == uint32_t(uintptr_t(small) - g_window));
  void *host = native::allocHost(1 << 20, 16384);
  CHECK(host && !inWindow(host) && (uintptr_t(host) & 16383) == 0);

  /* a thread without a window, and a native thread: the native heap */
  std::atomic<void *> no_window{nullptr}, from_native{nullptr};
  std::thread plain([&]() {
    t_no_window = true;
    no_window = dxmt::wsi::aligned_malloc(64, 16);
  });
  plain.join();
  dxmt::thread frontend([&]() {
    from_native = dxmt::wsi::aligned_malloc(64, 16);
    dxmt::wsi::aligned_free(mirror); /* window memory freed on a native thread */
  });
  frontend.join();
  CHECK(no_window.load() && !inWindow(no_window.load()));
  CHECK(from_native.load() && !inWindow(from_native.load()));
  for (void *p : {after, small, host, no_window.load(), from_native.load()})
    dxmt::wsi::aligned_free(p);
  dxmt::wsi::aligned_free(nullptr);
  ios_wow_window_stats stats;
  ios_wow_window_get_stats(&stats);
  CHECK(stats.allocations == 0);
}

static void
testWrappers() {
  RECT rc{};
  CHECK(GetClientRect(reinterpret_cast<HWND>(0x10020), &rc));
  CHECK(rc.left == 1 && rc.top == 2 && rc.right == 641 && rc.bottom == 482);
  CHECK(!GetClientRect(reinterpret_cast<HWND>(0x10024), &rc));
  CHECK(!GetClientRect(reinterpret_cast<HWND>(0x10020), nullptr));
  POINT pt = {1, 1};
  CHECK(ClientToScreen(reinterpret_cast<HWND>(0x10020), &pt) && pt.x == 8 && pt.y == 10);
  CHECK(IsWindow(reinterpret_cast<HWND>(0x10020)) && !IsWindow(nullptr));
  CHECK(!IsWindowVisible(reinterpret_cast<HWND>(0x10020)));
  CHECK(GetSystemMetrics(SM_CXSCREEN) == 1024 && GetSystemMetrics(SM_CYSCREEN) == 768);
  CHECK(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY) == reinterpret_cast<HMONITOR>(0x101));
  CHECK(MonitorFromPoint(POINT{3, 4}, MONITOR_DEFAULTTONEAREST) == reinterpret_cast<HMONITOR>(0x209));
  MONITORINFO mi{};
  CHECK(GetMonitorInfoW(reinterpret_cast<HMONITOR>(0x101), &mi));
  CHECK(mi.rcMonitor.right == 1024 && mi.rcWork.bottom == 740 && mi.dwFlags == 1);
  DEVMODEW dm{};
  CHECK(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm));
  CHECK(dm.dmPelsWidth == 1024 && dm.dmPelsHeight == 768 && dm.dmBitsPerPel == 32 && dm.dmDisplayFrequency == 60);
  CHECK((dm.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) == (DM_PELSWIDTH | DM_PELSHEIGHT));
  CHECK(!EnumDisplaySettingsW(nullptr, 0, &dm));
  CHECK(!EnumDisplaySettingsExW(L"\\\\.\\DISPLAY2", ENUM_CURRENT_SETTINGS, &dm, 0));
}

static void
testObjectHooks() {
  namespace native = dxmt::ios_wow_native;
  const uint64_t live0 = native::liveObjects();
  const size_t gone0 = g_gone.size();
  auto *object = new TestObject();
  CHECK(native::liveObjects() == live0 + 1);
  IUnknown *handed_out = object;
  CHECK(handed_out->AddRef() == 1);
  object->AddRefPrivate();
  CHECK(handed_out->Release() == 0); /* a private reference keeps it */
  CHECK(g_gone.size() == gone0);
  object->ReleasePrivate();
  CHECK(g_gone.size() == gone0 + 1);
  CHECK(!g_gone.empty() && g_gone.back() == static_cast<void *>(handed_out));
  CHECK(native::liveObjects() == live0);

  /* clarification 13: orphans stay alive, stop counting, and their later end changes nothing
     of the next generation's count */
  auto *old_a = new TestObject(), *old_b = new TestObject();
  old_a->AddRef();
  old_b->AddRef();
  const uint64_t all = native::allLiveObjects();
  CHECK(native::orphanLiveObjects() == live0 + 2);
  CHECK(native::liveObjects() == 0);
  CHECK(native::allLiveObjects() == all); /* nothing destroyed */
  CHECK(g_gone.size() == gone0 + 1);
  auto *fresh = new TestObject();
  fresh->AddRef();
  CHECK(native::liveObjects() == 1);
  CHECK(old_a->Release() == 0); /* an orphan destroyed later */
  CHECK(g_gone.size() == gone0 + 2 && g_gone.back() == static_cast<void *>(static_cast<IUnknown *>(old_a)));
  CHECK(native::liveObjects() == 1);
  CHECK(native::allLiveObjects() == all);
  CHECK(native::orphanLiveObjects() == 1); /* a second end: only the fresh object */
  CHECK(old_b->Release() == 0 && fresh->Release() == 0);
  CHECK(native::liveObjects() == 0);
  CHECK(native::allLiveObjects() == all - 2);
}

static void
testFocusActions() {
  using namespace dxmt::d9native;
  FocusInput in{};
  in.may_touch_window = true;
  in.has_device_window = true;
  in.backbuffer_width = 800;
  in.backbuffer_height = 600;
  /* losing focus: minimize only a visible window */
  in.activated = false;
  in.window_visible = true;
  CHECK(FocusWindowActions(in).actions == kFocusMinimize);
  in.window_visible = false;
  CHECK(FocusWindowActions(in).actions == 0);
  /* gaining focus: move to the monitor origin at the back buffer size */
  in.activated = true;
  in.monitor_x = 1280;
  in.monitor_y = 0;
  FocusActions out = FocusWindowActions(in);
  CHECK(out.actions == kFocusMove);
  CHECK(out.pos.left == 1280 && out.pos.top == 0 && out.pos.right == 2080 && out.pos.bottom == 600);
  /* D3DCREATE_NOWINDOWCHANGES, or no device window: nothing */
  in.may_touch_window = false;
  CHECK(FocusWindowActions(in).actions == 0);
  in.may_touch_window = true;
  in.has_device_window = false;
  CHECK(FocusWindowActions(in).actions == 0);
  in.activated = false;
  in.window_visible = true;
  CHECK(FocusWindowActions(in).actions == 0);
}

int
main() {
  void *r = mmap(nullptr, size_t(8) << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (r == MAP_FAILED)
    return 1;
  g_window = (uintptr_t(r) + 0xffffffffull) & ~uintptr_t(0xffffffffull);
  g_next = g_window + 0x10000;

  testThreads();
  testLocks();
  testAddressWait();
  testMemory();
  testWrappers();
  testObjectHooks();
  testFocusActions();
  std::printf("test_ios_wow_native_platform: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
