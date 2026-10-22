// Changed for MYIOSDECK (spec 07 A2): foregroundWindow, an LGPL-2.1-or-later addition from
// dxmt-d3d9 v0.4-d3d9 (e8dd4c6), marked inline (see LICENSE-NOTES.md).
/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include "wsi_window.hpp"
#include "wsi_monitor.hpp"

#include "util_string.hpp"
#include "log/log.hpp"

namespace dxmt::wsi {

void getWindowSize(HWND hWindow, uint32_t *pWidth, uint32_t *pHeight) {
  if (pWidth)
    *pWidth = 1024;

  if (pHeight)
    *pHeight = 1024;
}

void resizeWindow(HWND hWindow, DXMTWindowState *pState, uint32_t width,
                  uint32_t height) {
}

bool setWindowMode(HMONITOR hMonitor, HWND hWindow, const WsiMode &mode) {
  return false;
}

bool enterFullscreenMode(HMONITOR hMonitor, HWND hWindow,
                         DXMTWindowState *pState,
                         [[maybe_unused]] bool modeSwitch) {
  return false;
}

bool leaveFullscreenMode(HWND hWindow, DXMTWindowState *pState,
                         bool restoreCoordinates) {
  return false;
}

bool restoreDisplayMode(HMONITOR hMonitor) {
  return false;
}

HMONITOR getWindowMonitor(HWND hWindow) {
  return 0;
}

bool isWindow(HWND hWindow) { return true; }

void updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow,
                            bool forceTopmost) {
}

bool isForeground(HWND hWindow) { return true; }

bool isMinimized(HWND hWindow) { return false; }

/* MYIOSDECK spec 07: LGPL-2.1-or-later, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6) */
HWND foregroundWindow() { return nullptr; }

} // namespace dxmt::wsi
