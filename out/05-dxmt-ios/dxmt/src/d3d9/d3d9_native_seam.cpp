// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 clarification 7: the native frontend's side of interfaces/d3d9w_native.h, the seam
// between the ios_wow_native build of this frontend and its dispatch (out/33-d3d9-native-wow64).
//   - d3d9w_native_create9 / d3d9w_native_create9ex: the native halves of Direct3DCreate9(Ex),
//     refused while objects of another process exist (clarification 10: one process at a time;
//     the thunk then takes the full frontend, spec 33 H1).
//   - d3d9w_native_window_state / d3d9w_native_focus_activation: the device's window state and
//     the device side of WM_ACTIVATEAPP (MTLD3D9Device::nativeWindowState and
//     nativeFocusActivation, d3d9_device.cpp).
//   - d3d9w_object_gone, which the dispatch defines, is called from util/com's ComObject
//     destructor for every object destroyed (util_ios_wow_native.cpp).
//   - d3d9w_native_process_gone: the end of the process the frontend serves orphans its live
//     objects (clarification 13).

#ifdef DXMT_IOS_WOW_NATIVE

#include "d3d9w_native.h"

#include "d3d9_device.hpp"
#include "d3d9_native_focus.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion);
extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D);

static_assert(D3D9W_FOCUS_MINIMIZE == dxmt::d9native::kFocusMinimize, "focus action bits disagree");
static_assert(D3D9W_FOCUS_MOVE == dxmt::d9native::kFocusMove, "focus action bits disagree");

namespace {

std::mutex g_owner_mutex;
unsigned long g_owner_base = 0;
std::atomic<uint32_t> g_refusals{0};

std::string
hexBase(unsigned long base) {
  char text[32];
  std::snprintf(text, sizeof(text), "%#lx", base);
  return text;
}

/* Clarification 10. The native frontend is linked once into the app, which runs every Wine
   process: it serves the process that owns its live objects. Called with g_owner_mutex held,
   on a game thread inside a gate call. */
bool
mayServeThisProcess(const char *what) {
  const unsigned long base = ios_wow_base();
  if (dxmt::ios_wow_native::liveObjects() == 0 || g_owner_base == base) {
    if (g_owner_base != base)
      dxmt::Logger::info(dxmt::str::format("d3d9 (native): serving the 32-bit process at window ", hexBase(base)));
    g_owner_base = base;
    return true;
  }
  if (g_refusals.fetch_add(1, std::memory_order_relaxed) < 16)
    dxmt::Logger::warn(dxmt::str::format(
        "d3d9 (native): ", what, " refused: objects of the process at window ", hexBase(g_owner_base),
        " still exist (one process at a time); the process at window ", hexBase(base),
        " takes the full frontend"
    ));
  return false;
}

dxmt::MTLD3D9Device *
nativeDevice(IDirect3DDevice9 *device) {
  if (!device)
    return nullptr;
  // Every device this frontend hands out is an MTLD3D9Device, which implements
  // IDirect3DDevice9Ex whether or not it was created as an Ex device.
  return static_cast<dxmt::MTLD3D9Device *>(static_cast<IDirect3DDevice9Ex *>(device));
}

} // namespace

IDirect3D9 *
d3d9w_native_create9(UINT sdk_version) {
  std::lock_guard<std::mutex> lock(g_owner_mutex);
  if (!mayServeThisProcess("Direct3DCreate9"))
    return nullptr;
  return Direct3DCreate9(sdk_version);
}

HRESULT
d3d9w_native_create9ex(UINT sdk_version, IDirect3D9Ex **out) {
  if (!out)
    return D3DERR_INVALIDCALL;
  *out = nullptr;
  std::lock_guard<std::mutex> lock(g_owner_mutex);
  if (!mayServeThisProcess("Direct3DCreate9Ex"))
    return D3DERR_NOTAVAILABLE;
  return Direct3DCreate9Ex(sdk_version, out);
}

HRESULT
d3d9w_native_window_state(IDirect3DDevice9 *device, struct d3d9w_window_state *out) {
  dxmt::MTLD3D9Device *native = nativeDevice(device);
  if (!native || !out)
    return D3DERR_INVALIDCALL;
  return native->nativeWindowState(out);
}

DWORD
d3d9w_native_focus_activation(IDirect3DDevice9 *device, BOOL active, RECT *pos) {
  dxmt::MTLD3D9Device *native = nativeDevice(device);
  if (!native)
    return 0;
  return native->nativeFocusActivation(active != FALSE, pos);
}

/* Clarification 13: the calling process is ending (the dispatch calls this for the gate's
   reserved method 0xffffff, on the thread that makes the process's final NtTerminateProcess).
   If it is the process the frontend serves, every object still alive becomes an orphan: nothing
   is released or destroyed, since the process's other threads may have stopped anywhere, even
   inside the frontend; its threads stay parked and its memory stays. The next process's
   Direct3DCreate9(Ex) takes the native frontend. A process the frontend doesn't serve (one that
   took the full frontend, clarification 10) owns none of its objects: nothing changes. */
void
d3d9w_native_process_gone(void) {
  std::lock_guard<std::mutex> lock(g_owner_mutex);
  const unsigned long base = ios_wow_base(); /* a game thread: the ending process's */
  if (!g_owner_base || base != g_owner_base)
    return;
  const uint64_t orphaned = dxmt::ios_wow_native::orphanLiveObjects();
  g_owner_base = 0;
  dxmt::Logger::info(dxmt::str::format(
      "d3d9 (native): a process ended with ", orphaned, " objects alive; they are left as they are"
  ));
}

#endif /* DXMT_IOS_WOW_NATIVE */
