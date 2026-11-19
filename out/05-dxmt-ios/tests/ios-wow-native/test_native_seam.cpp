// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 clarification 6: session N's seam against a fake caller, on Linux. This program is
// linked with the whole ios_wow_native library (compile-native-linux.sh) and the fakes of
// native_fakes_wine.c, so the link itself proves the build calls nothing the app does not
// provide. It then calls interfaces/d3d9w_native.h as the dispatch would:
//   - d3d9w_native_create9 / create9ex give live objects, and d3d9w_object_gone is called once
//     per object destroyed, with the interface address handed out;
//   - clarification 10: while objects of one process (guest window) exist, another process is
//     refused, and served again once they are gone;
//   - clarification 13: after d3d9w_native_process_gone of the served process, a create from
//     another window succeeds while the old objects still exist (and the end of a process the
//     frontend doesn't serve changes nothing);
//   - the window-state and focus calls refuse a NULL device;
// and checks the platform layer in place: B4's queries reach win32u, memory the program may
// see comes from the guest window on a game thread and from the native heap on the frontend's
// own threads, which run at QoS user-interactive with their own thread ids; the device lock and
// the atomic wait work across those threads.

#include "d3d9w_native.h"

#include "airconv_public.h"
#include "d3d9_multithread.hpp"
#include "thread.hpp"
#include "util_atomic_wait.hpp"
#include "wsi_monitor.hpp"
#include "wsi_platform.hpp"
#include "wsi_window.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
void fake_set_window(int which);
uintptr_t fake_window_base(int which);
unsigned long fake_unix_calls(void);
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

/* ---- the dispatch's side of the seam ---- */

static std::vector<void *> g_gone;
extern "C" void
d3d9w_object_gone(void *iface) {
  g_gone.push_back(iface);
}

/* ---- the DXSO translator (airconv, in the unix half on iOS): nothing compiles here ---- */

extern "C" {
int
DXSOInitialize(const void *, size_t, dxso_shader_t *ppShader) {
  *ppShader = 0;
  return -1;
}
void
DXSODestroy(dxso_shader_t) {}
int
DXSOCompile(dxso_shader_t, struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *, const char *, dxso_bitcode_t *ppBitcode) {
  *ppBitcode = 0;
  return -1;
}
void
DXSOGetCompiledBitcode(dxso_bitcode_t, struct SM50_COMPILED_BITCODE *pData) {
  std::memset(pData, 0, sizeof(*pData));
}
void
DXSODestroyBitcode(dxso_bitcode_t) {}
}

static const HWND kWindow = reinterpret_cast<HWND>(uintptr_t(0x10020));

static void
testSeam() {
  fake_set_window(1);
  const size_t gone0 = g_gone.size();

  IDirect3D9 *d3d = d3d9w_native_create9(D3D_SDK_VERSION);
  CHECK(d3d != nullptr);
  if (!d3d)
    return;
  CHECK(d3d->GetAdapterCount() == 0); /* the fake unix side has no Metal device */
  IDirect3D9 *same = nullptr;
  CHECK(d3d->QueryInterface(__uuidof(IDirect3D9), reinterpret_cast<void **>(&same)) == S_OK);
  CHECK(same == d3d);
  if (same)
    same->Release();
  CHECK(g_gone.size() == gone0);

  /* clarification 10: another process while this one's objects exist */
  fake_set_window(2);
  CHECK(d3d9w_native_create9(D3D_SDK_VERSION) == nullptr);
  IDirect3D9Ex *refused = reinterpret_cast<IDirect3D9Ex *>(uintptr_t(1));
  CHECK(d3d9w_native_create9ex(D3D_SDK_VERSION, &refused) == D3DERR_NOTAVAILABLE);
  CHECK(refused == nullptr);
  CHECK(g_gone.size() == gone0);

  /* the same process again */
  fake_set_window(1);
  IDirect3D9Ex *ex = nullptr;
  CHECK(d3d9w_native_create9ex(D3D_SDK_VERSION, &ex) == D3D_OK);
  CHECK(ex != nullptr);
  CHECK(d3d9w_native_create9ex(D3D_SDK_VERSION, nullptr) == D3DERR_INVALIDCALL);
  IDirect3D9 *base = nullptr;
  if (ex) {
    CHECK(ex->QueryInterface(__uuidof(IDirect3D9), reinterpret_cast<void **>(&base)) == S_OK);
    CHECK(static_cast<void *>(base) == static_cast<void *>(ex));
    if (base)
      base->Release();
  }

  /* destruction: d3d9w_object_gone once each, with the handed-out address */
  CHECK(d3d->Release() == 0);
  CHECK(g_gone.size() == gone0 + 1);
  CHECK(!g_gone.empty() && g_gone.back() == static_cast<void *>(d3d));
  if (ex) {
    CHECK(ex->Release() == 0);
    CHECK(g_gone.size() == gone0 + 2);
    CHECK(!g_gone.empty() && g_gone.back() == static_cast<void *>(ex));
  }

  /* all gone: the other process is served now */
  fake_set_window(2);
  IDirect3D9 *other = d3d9w_native_create9(D3D_SDK_VERSION);
  CHECK(other != nullptr);
  if (other) {
    CHECK(other->Release() == 0);
    CHECK(g_gone.size() == gone0 + 3);
  }
  fake_set_window(1);

  /* no device */
  d3d9w_window_state state;
  CHECK(d3d9w_native_window_state(nullptr, &state) == D3DERR_INVALIDCALL);
  RECT pos = {1, 2, 3, 4};
  CHECK(d3d9w_native_focus_activation(nullptr, TRUE, &pos) == 0);
  CHECK(pos.left == 1 && pos.bottom == 4);

  /* the unix side was reached through its 64-bit table */
  CHECK(fake_unix_calls() > 0);
}

/* Clarification 13: the end of the process the frontend serves orphans its objects */
static void
testProcessGone() {
  fake_set_window(1);
  IDirect3D9 *old_d3d = d3d9w_native_create9(D3D_SDK_VERSION);
  IDirect3D9Ex *old_ex = nullptr;
  CHECK(old_d3d != nullptr);
  CHECK(d3d9w_native_create9ex(D3D_SDK_VERSION, &old_ex) == D3D_OK && old_ex);
  if (!old_d3d || !old_ex)
    return;

  /* a process the frontend doesn't serve ends: nothing changes */
  fake_set_window(2);
  d3d9w_native_process_gone();
  CHECK(d3d9w_native_create9(D3D_SDK_VERSION) == nullptr);

  /* the served process ends: its objects stay alive and stop counting */
  fake_set_window(1);
  const size_t gone0 = g_gone.size();
  d3d9w_native_process_gone();
  CHECK(g_gone.size() == gone0); /* nothing released or destroyed */
  CHECK(dxmt::ios_wow_native::liveObjects() == 0);
  CHECK(dxmt::ios_wow_native::allLiveObjects() >= 2);
  CHECK(old_d3d->GetAdapterCount() == 0); /* still usable */

  /* the next process takes the native frontend while the old objects exist */
  fake_set_window(2);
  IDirect3D9 *next = d3d9w_native_create9(D3D_SDK_VERSION);
  CHECK(next != nullptr);
  fake_set_window(1);
  CHECK(d3d9w_native_create9(D3D_SDK_VERSION) == nullptr); /* now the other process's turn */

  /* an orphan destroyed later is still reported once, and doesn't free the new owner's claim */
  CHECK(old_d3d->Release() == 0);
  CHECK(g_gone.size() == gone0 + 1 && g_gone.back() == static_cast<void *>(old_d3d));
  CHECK(dxmt::ios_wow_native::liveObjects() >= 1);
  CHECK(d3d9w_native_create9(D3D_SDK_VERSION) == nullptr);
  CHECK(old_ex->Release() == 0);
  CHECK(g_gone.size() == gone0 + 2);
  if (next) {
    fake_set_window(2);
    CHECK(next->Release() == 0);
  }
  /* all gone: either process may be served again */
  fake_set_window(1);
  IDirect3D9 *again = d3d9w_native_create9(D3D_SDK_VERSION);
  CHECK(again != nullptr);
  if (again)
    again->Release();
}

/* B4: the allowed queries, through util_ios_wow_native_user.c to (fake) win32u */
static void
testQueries() {
  RECT rc{};
  CHECK(GetClientRect(kWindow, &rc));
  CHECK(rc.left == 0 && rc.top == 0 && rc.right == 800 && rc.bottom == 600);
  CHECK(!GetClientRect(nullptr, &rc));
  POINT pt = {10, 10};
  CHECK(ClientToScreen(kWindow, &pt));
  CHECK(pt.x == 110 && pt.y == 60);
  CHECK(IsWindow(kWindow));
  CHECK(!IsWindow(reinterpret_cast<HWND>(uintptr_t(0x10030))));
  CHECK(IsWindowVisible(kWindow));
  CHECK(GetSystemMetrics(SM_CXSCREEN) == 1280 && GetSystemMetrics(SM_CYSCREEN) == 720);
  HMONITOR monitor = MonitorFromWindow(kWindow, MONITOR_DEFAULTTOPRIMARY);
  CHECK(monitor == reinterpret_cast<HMONITOR>(uintptr_t(0x10001)));
  CHECK(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY) == monitor);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  CHECK(GetMonitorInfoW(monitor, &mi));
  CHECK(mi.rcMonitor.right == 1280 && mi.rcMonitor.bottom == 720 && mi.rcWork.bottom == 700);
  DEVMODEW dm{};
  CHECK(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm));
  CHECK(dm.dmPelsWidth == 1280 && dm.dmPelsHeight == 720 && dm.dmBitsPerPel == 32 && dm.dmDisplayFrequency == 60);
  CHECK(EnumDisplaySettingsExW(nullptr, 0, &dm, 0) && dm.dmPelsWidth == 640);
  CHECK(!EnumDisplaySettingsExW(nullptr, 3, &dm, 0));

  /* spec 05 E1's window system on top of them */
  uint32_t w = 0, h = 0;
  dxmt::wsi::getWindowSize(kWindow, &w, &h);
  CHECK(w == 800 && h == 600);
  dxmt::wsi::WsiMode mode{};
  CHECK(dxmt::wsi::getCurrentDisplayMode(dxmt::wsi::getDefaultMonitor(), &mode));
  CHECK(mode.width == 1280 && mode.height == 720);
}

/* Part C through wsi::aligned_malloc, and B3's threads */
static void
testMemoryAndThreads() {
  namespace native = dxmt::ios_wow_native;
  fake_set_window(1);
  const uintptr_t window = fake_window_base(1);

  /* a game thread: the guest window, 16 KB aligned and whole 16 KB pages for page alignment */
  CHECK(!native::isNativeThread());
  void *mirror = dxmt::wsi::aligned_malloc(5000, DXMT_PAGE_SIZE);
  CHECK(mirror != nullptr);
  CHECK(uintptr_t(mirror) - window < 0x100000000ull);
  CHECK((uintptr_t(mirror) & 16383) == 0);
  CHECK(ios_wow_window_guest(mirror) == uint32_t(uintptr_t(mirror) - window));
  void *next = dxmt::wsi::aligned_malloc(16, DXMT_PAGE_SIZE);
  CHECK(uintptr_t(next) >= uintptr_t(mirror) + 16384); /* 5000 rounded to a 16 KB page */
  void *small = dxmt::wsi::aligned_malloc(100, 64);
  CHECK(small && (uintptr_t(small) & 63) == 0 && ios_wow_window_owns(small));
  void *host = native::allocHost(100, 64);
  CHECK(host && !ios_wow_window_owns(host));
  dxmt::wsi::aligned_free(host);

  /* a frontend thread: native, user-interactive, its own id, no window memory; it may free */
  std::atomic<bool> was_native{false};
  std::atomic<int> qos{0};
  std::atomic<uint32_t> tid{0};
  std::atomic<void *> from_thread{nullptr};
  uint32_t main_tid = GetCurrentThreadId();
  dxmt::thread t([&]() {
    was_native = native::isNativeThread();
    qos = native::currentThreadQos();
    tid = GetCurrentThreadId();
    from_thread = dxmt::wsi::aligned_malloc(4096, DXMT_PAGE_SIZE);
    dxmt::wsi::aligned_free(mirror); /* frees from the encode / finish threads are allowed */
  });
  t.join();
  CHECK(was_native.load());
  CHECK(qos.load() == native::kQosUserInteractive);
  CHECK(tid.load() != 0 && main_tid != 0 && tid.load() != main_tid);
  CHECK(GetCurrentThreadId() == main_tid);
  CHECK(from_thread.load() != nullptr && !ios_wow_window_owns(from_thread.load()));
  dxmt::wsi::aligned_free(from_thread.load());
  dxmt::wsi::aligned_free(next);
  dxmt::wsi::aligned_free(small);

  /* SetThreadPriority (util_win32_compat.h) on the calling thread maps to QoS */
  CHECK(native::qosForPriority(THREAD_PRIORITY_TIME_CRITICAL) == native::kQosUserInteractive);
  CHECK(native::qosForPriority(THREAD_PRIORITY_NORMAL) == native::kQosUserInteractive);
  CHECK(native::qosForPriority(THREAD_PRIORITY_LOWEST) == native::kQosUtility);
  CHECK(native::qosForPriority(THREAD_PRIORITY_IDLE) == native::kQosBackground);

  /* the device lock: recursive on one thread, exclusive across threads */
  dxmt::D9RecursiveSpinlock lock;
  lock.lock();
  CHECK(lock.try_lock()); /* recursion */
  std::atomic<bool> other_got{true};
  dxmt::thread locker([&]() { other_got = lock.try_lock(); });
  locker.join();
  CHECK(!other_got.load());
  lock.unlock();
  lock.unlock();
  dxmt::thread locker2([&]() {
    other_got = lock.try_lock();
    if (other_got)
      lock.unlock();
  });
  locker2.join();
  CHECK(other_got.load());

  /* the address wait (clarification 9: std::atomic's wait) across a frontend thread */
  std::atomic<uint32_t> word{0};
  std::atomic<bool> woke{false};
  dxmt::thread waiter([&]() {
    dxmt::atomic_wait(word, 0u);
    woke = true;
  });
  for (int i = 0; i < 1000 && word.load() == 0; i++)
    SwitchToThread();
  word.store(1);
  dxmt::atomic_notify_all(word);
  waiter.join();
  CHECK(woke.load());
  dxmt::atomic_wait(word, 0u); /* already changed: returns at once */
}

int
main() {
  testSeam();
  testProcessGone();
  testQueries();
  testMemoryAndThreads();
  std::printf("test_native_seam: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
