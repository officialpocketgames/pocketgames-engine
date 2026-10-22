// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 E1: windows for the app's Wine, which has no window manager. The
// window is always on the one monitor, never minimized and always in the
// foreground; fullscreen and display-mode changes succeed without doing
// anything.

#include "wsi_window.hpp"
#include "wsi_monitor.hpp"

namespace dxmt::wsi {

void
getWindowSize(HWND hWindow, uint32_t *pWidth, uint32_t *pHeight) {
  RECT rect = {};
  uint32_t width = 0, height = 0;
  if (::GetClientRect(hWindow, &rect) && rect.right > rect.left && rect.bottom > rect.top) {
    width = uint32_t(rect.right - rect.left);
    height = uint32_t(rect.bottom - rect.top);
  } else {
    UINT w = 0, h = 0;
    getMonitorClientSize(getDefaultMonitor(), &w, &h);
    width = w;
    height = h;
  }
  if (pWidth)
    *pWidth = width;
  if (pHeight)
    *pHeight = height;
}

void
resizeWindow(HWND hWindow, DXMTWindowState *pState, uint32_t width, uint32_t height) {}

bool
setWindowMode(HMONITOR hMonitor, HWND hWindow, const WsiMode &mode) {
  return true;
}

bool
enterFullscreenMode(HMONITOR hMonitor, HWND hWindow, DXMTWindowState *pState, [[maybe_unused]] bool modeSwitch) {
  return true;
}

bool
leaveFullscreenMode(HWND hWindow, DXMTWindowState *pState, bool restoreCoordinates) {
  return true;
}

bool
restoreDisplayMode(HMONITOR hMonitor) {
  return true;
}

HMONITOR
getWindowMonitor(HWND hWindow) {
  return getDefaultMonitor();
}

bool
isWindow(HWND hWindow) {
  return ::IsWindow(hWindow);
}

void
updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow, bool forceTopmost) {}

bool
isForeground(HWND hWindow) {
  return true;
}

bool
isMinimized(HWND hWindow) {
  return false;
}

/* Spec 07 (frontend focus poll): always the same answer, so no focus change
   is ever seen (spec 05 E1: always foreground). */
HWND
foregroundWindow() {
  return nullptr;
}

} // namespace dxmt::wsi
