// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 clarification 7: which window actions the focus window's WM_ACTIVATEAPP takes for a
// fullscreen device, as a pure function (host-tested). The PE build's onFocusActivation
// (d3d9_device.cpp) makes these calls itself:
//   - losing focus: ShowWindow(device window, SW_MINIMIZE) when window changes are allowed
//     (no D3DCREATE_NOWINDOWCHANGES) and the device window is visible;
//   - gaining focus: SetWindowPos(device window, NULL, monitor origin, back buffer size,
//     SWP_NOACTIVATE | SWP_NOZORDER) when window changes are allowed.
// In the ios_wow_native build the device returns them for the thunk to make in guest code
// (d3d9w_native_focus_activation, interfaces/d3d9w_native.h).

#pragma once

#include <windows.h>

#include <cstdint>

namespace dxmt::d9native {

/* The bits of d3d9w_native_focus_activation's result (D3D9W_FOCUS_MINIMIZE and
   D3D9W_FOCUS_MOVE; d3d9_native_seam.cpp checks they agree). */
constexpr DWORD kFocusMinimize = 0x1u;
constexpr DWORD kFocusMove = 0x2u;

struct FocusInput {
  bool activated;         // WM_ACTIVATEAPP's wParam
  bool may_touch_window;  // no D3DCREATE_NOWINDOWCHANGES
  bool has_device_window;
  bool window_visible;    // IsWindowVisible(device window), asked only when losing focus
  LONG monitor_x;         // origin of the monitor the device is fullscreen on (0, 0 if unknown)
  LONG monitor_y;
  UINT backbuffer_width;
  UINT backbuffer_height;
};

struct FocusActions {
  DWORD actions;
  RECT pos;  // for kFocusMove: left/top the origin, right/bottom = origin + back buffer size
};

constexpr FocusActions
FocusWindowActions(const FocusInput &in) {
  FocusActions out{};
  if (!in.may_touch_window || !in.has_device_window)
    return out;
  if (!in.activated) {
    if (in.window_visible)
      out.actions |= kFocusMinimize;
    return out;
  }
  out.actions |= kFocusMove;
  out.pos.left = in.monitor_x;
  out.pos.top = in.monitor_y;
  out.pos.right = in.monitor_x + static_cast<LONG>(in.backbuffer_width);
  out.pos.bottom = in.monitor_y + static_cast<LONG>(in.backbuffer_height);
  return out;
}

} // namespace dxmt::d9native
