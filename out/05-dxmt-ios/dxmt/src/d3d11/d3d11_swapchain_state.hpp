// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 round 19 (item 88): swap-chain and DXGI-device methods that used to be IMPLEMENT_ME,
// which logs and calls abort() (exit code 3), so a game library that calls one during set-up
// ended the game. Each now keeps what the game sets and gives it back, and changes nothing
// about how DXMT presents. Pure: the DXGI values are mirrored here as numbers (d3d11_swapchain.cpp
// and d3d11_device.cpp check them against the headers with static_assert), so the host test
// (tests/test_swapchain_state.cpp) builds it without Windows headers.

#pragma once

#include <atomic>
#include <cstdint>

namespace dxmt::swapchain_state {

/* What a method answers; the callers turn it into an HRESULT. */
enum class Result { Ok, InvalidArg };

/* ---- IDXGISwapChain1::SetRotation / GetRotation ---- */

/* DXGI_MODE_ROTATION (Microsoft's dxgitype.h) */
constexpr uint32_t kRotationUnspecified = 0;
constexpr uint32_t kRotationIdentity = 1;
constexpr uint32_t kRotationRotate90 = 2;
constexpr uint32_t kRotationRotate180 = 3;
constexpr uint32_t kRotationRotate270 = 4;

/* Item 88: SetRotation accepts every value. UNSPECIFIED is stored as IDENTITY; anything else
   is stored as given. Presenting never rotates: the iOS layer keeps its own orientation.
   GetRotation answers the stored value, IDENTITY before any set. */
class Rotation {
public:
  /* Stores `rotation`; true when the caller should log that it isn't applied (the first value
     other than IDENTITY or UNSPECIFIED that this `logged` flag sees: once per process). */
  bool
  set(uint32_t rotation, std::atomic<bool> &logged) {
    stored_ = rotation == kRotationUnspecified ? kRotationIdentity : rotation;
    if (stored_ == kRotationIdentity)
      return false;
    return !logged.exchange(true, std::memory_order_relaxed);
  }

  Result
  get(uint32_t *rotation) const {
    if (!rotation)
      return Result::InvalidArg;
    *rotation = stored_;
    return Result::Ok;
  }

private:
  uint32_t stored_ = kRotationIdentity;
};

/* ---- IDXGISwapChain1::SetBackgroundColor / GetBackgroundColor ---- */

/* DXGI_RGBA */
struct Rgba {
  float r = 0, g = 0, b = 0, a = 0;
};

/* Stored and answered back; DXMT's layer shows the whole back buffer, so there is no area for
   the colour to fill. Before any set: all zero. */
class BackgroundColor {
public:
  Result
  set(const Rgba *color) {
    if (!color)
      return Result::InvalidArg;
    stored_ = *color;
    return Result::Ok;
  }

  Result
  get(Rgba *color) const {
    if (!color)
      return Result::InvalidArg;
    *color = stored_;
    return Result::Ok;
  }

private:
  Rgba stored_;
};

/* ---- IDXGISwapChain2::SetSourceSize / GetSourceSize ---- */

/* The region of the back buffer the game draws into. Microsoft documents E_INVALIDARG for a
   size of 0 or larger than the buffers. A valid size is stored and answered back; DXMT still
   presents the whole buffer. Before any set, and after ResizeBuffers (reset()), the answer is
   the buffers' own size. */
class SourceSize {
public:
  Result
  set(uint32_t width, uint32_t height, uint32_t buffer_width, uint32_t buffer_height) {
    if (width == 0 || height == 0 || width > buffer_width || height > buffer_height)
      return Result::InvalidArg;
    width_ = width;
    height_ = height;
    set_ = true;
    return Result::Ok;
  }

  /* Either pointer may be null (only the other is written); both null is an error. */
  Result
  get(uint32_t *width, uint32_t *height, uint32_t buffer_width, uint32_t buffer_height) const {
    if (!width && !height)
      return Result::InvalidArg;
    if (width)
      *width = set_ ? width_ : buffer_width;
    if (height)
      *height = set_ ? height_ : buffer_height;
    return Result::Ok;
  }

  void
  reset() {
    set_ = false;
  }

private:
  uint32_t width_ = 0, height_ = 0;
  bool set_ = false;
};

/* ---- IDXGIDevice::QueryResourceResidency ---- */

/* DXGI_RESIDENCY (Microsoft's dxgi.h) */
constexpr uint32_t kResidencyFullyResident = 1;

/* Metal on these phones has no eviction DXGI could report: every resource is fully resident.
   Null arrays with a count are an error. */
inline Result
QueryResidency(const void *const *resources, uint32_t *residency, uint32_t count) {
  if (count && (!resources || !residency))
    return Result::InvalidArg;
  for (uint32_t i = 0; i < count; i++)
    residency[i] = kResidencyFullyResident;
  return Result::Ok;
}

} // namespace dxmt::swapchain_state
