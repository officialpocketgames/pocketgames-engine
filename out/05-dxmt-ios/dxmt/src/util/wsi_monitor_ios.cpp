// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 E1: monitors for the app's Wine, which has no window manager.
// Exactly one monitor; its size is the virtual display's (spec 02), as user32
// reports it, so DXGI and user32 agree.

#include "wsi_monitor.hpp"
#include "util_bitness.hpp"
#include "util_ios_switch.hpp"

#include <algorithm>
#include <vector>

namespace dxmt::wsi {

namespace {

/* DXMT_WSI_MONITOR_IDENTITY (default on in the i386 build only, spec 06 A):
   use user32's primary monitor handle everywhere instead of a fixed one. */
bool
useMonitorIdentity() {
  static const bool on = env::readSwitch("DXMT_WSI_MONITOR_IDENTITY", kI386Build);
  return on;
}

/* DXMT_WSI_MODE_TABLE (default on in the i386 build only, spec 06 A): list
   the modes user32 lists. */
bool
useModeTable() {
  static const bool on = env::readSwitch("DXMT_WSI_MODE_TABLE", kI386Build);
  return on;
}

HMONITOR
theMonitor() {
  if (useMonitorIdentity()) {
    POINT origin = {0, 0};
    if (HMONITOR monitor = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY))
      return monitor;
  }
  return reinterpret_cast<HMONITOR>(uintptr_t(1));
}

void
screenSize(uint32_t &width, uint32_t &height) {
  int w = ::GetSystemMetrics(SM_CXSCREEN);
  int h = ::GetSystemMetrics(SM_CYSCREEN);
  if (w <= 0 || h <= 0) {
    w = 1024;
    h = 768;
  }
  width = uint32_t(w);
  height = uint32_t(h);
}

WsiMode
makeMode(uint32_t width, uint32_t height, uint32_t refresh = 60, uint32_t bpp = 32) {
  WsiMode mode = {};
  mode.width = width;
  mode.height = height;
  mode.refreshRate = {refresh, 1};
  mode.bitsPerPixel = bpp;
  mode.interlaced = false;
  return mode;
}

/* Default list: 640x480, 800x600 and the screen size, without duplicates. */
std::vector<WsiMode>
defaultModes() {
  uint32_t w, h;
  screenSize(w, h);
  std::vector<std::pair<uint32_t, uint32_t>> sizes = {{640, 480}, {800, 600}, {w, h}};
  std::sort(sizes.begin(), sizes.end());
  sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
  std::vector<WsiMode> modes;
  for (auto &size : sizes)
    modes.push_back(makeMode(size.first, size.second));
  return modes;
}

/* DXMT_WSI_MODE_TABLE: mode `index` as EnumDisplaySettingsExW(NULL, ...)
   lists it; refresh 0 or 1 reads as 60, bpp 0 as 32. */
bool
tableMode(uint32_t index, WsiMode *mode) {
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  if (!::EnumDisplaySettingsExW(nullptr, index, &dm, 0))
    return false;
  uint32_t refresh = dm.dmDisplayFrequency <= 1 ? 60 : dm.dmDisplayFrequency;
  uint32_t bpp = dm.dmBitsPerPel ? dm.dmBitsPerPel : 32;
  *mode = makeMode(dm.dmPelsWidth, dm.dmPelsHeight, refresh, bpp);
  return true;
}

} // namespace

HMONITOR
getDefaultMonitor() {
  return theMonitor();
}

HMONITOR
enumMonitors(uint32_t index) {
  return index == 0 ? theMonitor() : nullptr;
}

bool
getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) {
  if (hMonitor != theMonitor())
    return false;
  static const WCHAR name[] = L"\\\\.\\DISPLAY1";
  std::copy(std::begin(name), std::end(name), Name);
  return true;
}

bool
getDesktopCoordinates(HMONITOR hMonitor, RECT *pRect) {
  if (hMonitor != theMonitor() || !pRect)
    return false;
  uint32_t w, h;
  screenSize(w, h);
  *pRect = {0, 0, LONG(w), LONG(h)};
  return true;
}

bool
getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode *pMode) {
  if (hMonitor != theMonitor() || !pMode)
    return false;
  if (useModeTable())
    return tableMode(modeNumber, pMode);
  auto modes = defaultModes();
  if (modeNumber >= modes.size())
    return false;
  *pMode = modes[modeNumber];
  return true;
}

bool
getCurrentDisplayMode(HMONITOR hMonitor, WsiMode *pMode) {
  if (hMonitor != theMonitor() || !pMode)
    return false;
  uint32_t w, h;
  screenSize(w, h);
  *pMode = makeMode(w, h);
  return true;
}

bool
getDesktopDisplayMode(HMONITOR hMonitor, WsiMode *pMode) {
  return getCurrentDisplayMode(hMonitor, pMode);
}

} // namespace dxmt::wsi
