// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 Part B: the platform layer of the ios_wow_native build, which compiles the Direct3D 9
// frontend with the dxmt core and util as native ARM64 code for 32-bit programs. The build
// includes this header first in every C++ file (meson: -include), after DXMT's native Windows
// headers (include/native/windows, as the dxmt_native build uses them). It supplies:
//   - the few Windows types and constants the frontend names beyond those headers;
//   - B4's allowed unix-side queries under their user32 names (GetClientRect, ClientToScreen,
//     IsWindow, the monitor queries, GetSystemMetrics, the display-settings enumeration), as
//     wrappers over util_ios_wow_native_user.c; in debug builds they assert that they run on a
//     Wine (game) thread, never on one of the frontend's native threads;
//   - B3's thread mappings: GetCurrentThreadId is the pthread's id, YieldProcessor the ARM
//     "yield" hint, SwitchToThread sched_yield; the frontend's threads are plain pthreads at
//     QoS user-interactive;
//   - Part C's allocator (out/33-d3d9-native-wow64/alloc) for memory the program can see;
//   - the hooks util/com's ComObject calls when an object is made and destroyed (the seam's
//     d3d9w_object_gone, interfaces/d3d9w_native.h).
// Nothing here is used by any other build.

#pragma once

#ifdef DXMT_IOS_WOW_NATIVE

#ifndef __cdecl
#define __cdecl
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <sched.h>

#include "util_ios_wow_native_user.h"

/* ---- Windows types and constants the frontend names (window state it keeps, never calls) ---- */

typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef uintptr_t DWORD_PTR;
typedef HANDLE HCURSOR;
typedef HANDLE HICON;
typedef HANDLE HBITMAP;
typedef HANDLE HGDIOBJ;
#ifndef CALLBACK
#define CALLBACK
#endif
typedef LRESULT(CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagMONITORINFO {
  DWORD cbSize;
  RECT rcMonitor;
  RECT rcWork;
  DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

/* Not Windows' layout: only the fields the native build reads or writes. It never reaches Wine
   (util_ios_wow_native_user.c fills Wine's own DEVMODEW). */
typedef struct tagDEVMODEW {
  WORD dmSize;
  DWORD dmFields;
  DWORD dmBitsPerPel;
  DWORD dmPelsWidth;
  DWORD dmPelsHeight;
  DWORD dmDisplayFrequency;
} DEVMODEW;

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define MONITOR_DEFAULTTONULL 0x0u
#define MONITOR_DEFAULTTOPRIMARY 0x1u
#define MONITOR_DEFAULTTONEAREST 0x2u
#define DM_BITSPERPEL 0x00040000u
#define DM_PELSWIDTH 0x00080000u
#define DM_PELSHEIGHT 0x00100000u
#define DM_DISPLAYFREQUENCY 0x00400000u
#define CDS_FULLSCREEN 0x00000004u
#define DISP_CHANGE_SUCCESSFUL 0
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_TIME_CRITICAL 15 /* as util_win32_compat.h has it */

/* ---- interfaces of other parts, linked into the same binary ---- */

extern "C" {
/* Spec 33 Part C (out/33-d3d9-native-wow64/alloc/ios_wow_window_alloc.h). */
void *ios_wow_window_alloc(size_t size, size_t align, uint32_t *guest);
void ios_wow_window_free(void *host);
int ios_wow_window_owns(const void *host);
uint32_t ios_wow_window_guest(const void *host);
void ios_wow_window_collect(void);
void ios_wow_window_mark_native_thread(void);
int ios_wow_window_is_native_thread(void);
/* Spec 06: B of the calling thread's program; 0 if it has none. */
unsigned long ios_wow_base(void);
/* The seam (interfaces/d3d9w_native.h): defined by the dispatch, weak here so the frontend's own
   tests link without one. */
void d3d9w_object_gone(void *iface) __attribute__((weak));
/* Wine's unix-side debug output (dlls/ntdll/unix/debug.c). */
int __wine_dbg_output(const char *str) __attribute__((weak));
/* The pthread id of the calling thread as GetCurrentThreadId reports it (0 until first asked). */
extern __thread uint32_t dxmt_native_thread_id;
}

namespace dxmt::ios_wow_native {

/* QoS classes, the values of Apple's qos_class_t (<sys/qos.h>). */
enum : int {
  kQosUserInteractive = 0x21,
  kQosUserInitiated = 0x19,
  kQosDefault = 0x15,
  kQosUtility = 0x11,
  kQosBackground = 0x09,
};

/* B3: a Windows thread priority as a QoS class. The frontend's threads ask for TIME_CRITICAL,
   which, like every priority from NORMAL up, is user-interactive. */
constexpr int
qosForPriority(int windows_priority) {
  return windows_priority >= THREAD_PRIORITY_NORMAL  ? kQosUserInteractive
         : windows_priority > THREAD_PRIORITY_IDLE ? kQosUtility
                                                     : kQosBackground;
}

/* Applies qosForPriority to the calling thread (pthread_set_qos_class_self_np on Darwin). */
void setCurrentThreadPriority(int windows_priority);
/* The QoS class last applied on the calling thread, 0 if none (tests). */
int currentThreadQos();
/* Runs first on every thread dxmt::thread starts: marks it native (Part C refuses to allocate
   there) and puts it at QoS user-interactive. */
void onNativeThreadStart();
bool isNativeThread();
/* B3, debug builds: a native thread must never call a Wine function. */
void wineThreadRequired(const char *what);
uint32_t initThreadId();

/* util/com's ComObject: every object made and destroyed. objectBorn returns the generation the
   object belongs to; objectGone calls the seam's d3d9w_object_gone with the interface address
   handed out for the object. */
uint32_t objectBorn();
void objectGone(void *iface, uint32_t generation);
/* COM objects of the current generation that exist now (clarification 10's ownership). */
uint64_t liveObjects();
/* COM objects that exist now, orphans included. */
uint64_t allLiveObjects();
/* Clarification 13: every object alive becomes an orphan (a new generation starts; nothing is
   released or destroyed). Returns how many there were. */
uint64_t orphanLiveObjects();

/* Memory the program may see (wsi::aligned_malloc in this build). On a game thread it comes
   from Part C's allocator, 16 KB aligned and a multiple of 16 KB when the caller asks for page
   alignment (it may be handed to Metal as a no-copy buffer); on a native thread, or when the
   window has no room, from the native heap, which the program never sees. */
void *allocVisible(size_t size, size_t alignment);
/* Frees memory from allocVisible or allocHost (wsi::aligned_free in this build). */
void freeVisible(void *ptr);
/* B2: memory only Metal and the frontend touch (staging rings, internal buffers): the native
   heap, on any thread. Freed with freeVisible / wsi::aligned_free. */
void *allocHost(size_t size, size_t alignment);

} // namespace dxmt::ios_wow_native

#ifndef NDEBUG
#define DXMT_NATIVE_WINE_THREAD(what) ::dxmt::ios_wow_native::wineThreadRequired(what)
#else
#define DXMT_NATIVE_WINE_THREAD(what) ((void)0)
#endif

/* ---- B3: thread primitives ---- */

static inline DWORD
GetCurrentThreadId() {
  uint32_t id = dxmt_native_thread_id;
  return id ? id : ::dxmt::ios_wow_native::initThreadId();
}

static inline void
YieldProcessor() {
#if defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause(); /* Linux host builds */
#endif
}

static inline BOOL
SwitchToThread() {
  return sched_yield() == 0;
}

/* ---- B4: the allowed unix-side queries, under their user32 names ---- */

static inline BOOL
GetClientRect(HWND hwnd, RECT *rect) {
  DXMT_NATIVE_WINE_THREAD("GetClientRect");
  dxmt_native_rect r;
  if (!rect || !dxmt_native_user_get_client_rect(hwnd, &r))
    return FALSE;
  *rect = RECT{r.left, r.top, r.right, r.bottom};
  return TRUE;
}

static inline BOOL
ClientToScreen(HWND hwnd, POINT *pt) {
  DXMT_NATIVE_WINE_THREAD("ClientToScreen");
  if (!pt)
    return FALSE;
  int32_t x = pt->x, y = pt->y;
  if (!dxmt_native_user_client_to_screen(hwnd, &x, &y))
    return FALSE;
  pt->x = x;
  pt->y = y;
  return TRUE;
}

static inline BOOL
IsWindow(HWND hwnd) {
  DXMT_NATIVE_WINE_THREAD("IsWindow");
  return hwnd && dxmt_native_user_is_window(hwnd);
}

static inline BOOL
IsWindowVisible(HWND hwnd) {
  DXMT_NATIVE_WINE_THREAD("IsWindowVisible");
  return hwnd && dxmt_native_user_is_window_visible(hwnd);
}

static inline int
GetSystemMetrics(int index) {
  DXMT_NATIVE_WINE_THREAD("GetSystemMetrics");
  return dxmt_native_user_get_system_metrics(index);
}

static inline HMONITOR
MonitorFromWindow(HWND hwnd, DWORD flags) {
  DXMT_NATIVE_WINE_THREAD("MonitorFromWindow");
  return static_cast<HMONITOR>(dxmt_native_user_monitor_from_window(hwnd, flags));
}

static inline HMONITOR
MonitorFromPoint(POINT pt, DWORD flags) {
  DXMT_NATIVE_WINE_THREAD("MonitorFromPoint");
  return static_cast<HMONITOR>(dxmt_native_user_monitor_from_point(pt.x, pt.y, flags));
}

static inline BOOL
GetMonitorInfoW(HMONITOR monitor, MONITORINFO *info) {
  DXMT_NATIVE_WINE_THREAD("GetMonitorInfoW");
  dxmt_native_rect mr, wr;
  uint32_t flags = 0;
  if (!info || !dxmt_native_user_get_monitor_info(monitor, &mr, &wr, &flags))
    return FALSE;
  info->rcMonitor = RECT{mr.left, mr.top, mr.right, mr.bottom};
  info->rcWork = RECT{wr.left, wr.top, wr.right, wr.bottom};
  info->dwFlags = flags;
  return TRUE;
}

/* Only the primary display (device NULL), as the frontend asks. */
static inline BOOL
EnumDisplaySettingsExW(LPCWSTR device, DWORD mode, DEVMODEW *dm, DWORD flags) {
  DXMT_NATIVE_WINE_THREAD("EnumDisplaySettingsExW");
  (void)flags;
  dxmt_native_display_mode m;
  if (device || !dm || !dxmt_native_user_enum_display_settings(mode, &m))
    return FALSE;
  dm->dmPelsWidth = m.width;
  dm->dmPelsHeight = m.height;
  dm->dmBitsPerPel = m.bits_per_pixel;
  dm->dmDisplayFrequency = m.frequency;
  dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
  return TRUE;
}

static inline BOOL
EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW *dm) {
  return EnumDisplaySettingsExW(device, mode, dm, 0);
}

#endif /* DXMT_IOS_WOW_NATIVE */
