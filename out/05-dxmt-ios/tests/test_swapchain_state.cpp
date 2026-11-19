// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 round 19 (item 88): the swap-chain and DXGI-device methods that used to abort
// (dxmt/src/d3d11/d3d11_swapchain_state.hpp).
//   - SetRotation / GetRotation: the default, a round trip for every DXGI_MODE_ROTATION value
//     (UNSPECIFIED reads back as IDENTITY), a value outside the enum, the null pointer, and the
//     log once per process for a rotation that isn't applied;
//   - the background colour, the source size (limits, default, reset by ResizeBuffers), the
//     residency query.
// That d3d11_swapchain.cpp maps Result::InvalidArg to E_INVALIDARG and that the mirrored
// numbers match Microsoft's headers is checked there with static_assert; only the device shows
// that SDL2's renderer now runs past SetRotation (TESTS.md device check 42).

#include "test_common.hpp"

#include "d3d11_swapchain_state.hpp"

#include <atomic>
#include <cstdint>

namespace {

using namespace dxmt::swapchain_state;

void
TestRotation() {
  std::atomic<bool> logged{false};
  Rotation r;
  uint32_t value = 99;
  CHECK(r.get(&value) == Result::Ok);
  CHECK_EQ(value, kRotationIdentity); // before any set

  // every enum value round-trips; UNSPECIFIED is stored as IDENTITY
  const uint32_t all[] = {kRotationUnspecified, kRotationIdentity, kRotationRotate90, kRotationRotate180,
                          kRotationRotate270};
  for (uint32_t set : all) {
    Rotation fresh;
    std::atomic<bool> fresh_logged{false};
    fresh.set(set, fresh_logged);
    CHECK(fresh.get(&value) == Result::Ok);
    CHECK_EQ(value, set == kRotationUnspecified ? kRotationIdentity : set);
    // identity and unspecified are applied as they are: nothing to log
    CHECK_EQ(fresh_logged.load(), set != kRotationUnspecified && set != kRotationIdentity);
  }

  // the log: once, at the first rotation that isn't applied, whatever follows
  CHECK(!r.set(kRotationIdentity, logged));
  CHECK(!r.set(kRotationUnspecified, logged));
  CHECK(r.set(kRotationRotate90, logged));
  CHECK(!r.set(kRotationRotate270, logged));
  CHECK(r.get(&value) == Result::Ok);
  CHECK_EQ(value, kRotationRotate270);
  CHECK(!r.set(kRotationIdentity, logged));
  CHECK(r.get(&value) == Result::Ok);
  CHECK_EQ(value, kRotationIdentity); // back to identity
  // a second swap chain shares the process's flag: no second line
  Rotation other;
  CHECK(!other.set(kRotationRotate180, logged));

  // a value outside the enum is stored as given (Set returns S_OK)
  Rotation odd;
  std::atomic<bool> odd_logged{false};
  odd.set(7, odd_logged);
  CHECK(odd.get(&value) == Result::Ok);
  CHECK_EQ(value, 7u);

  // the null pointer
  CHECK(r.get(nullptr) == Result::InvalidArg);
}

void
TestBackgroundColor() {
  BackgroundColor c;
  Rgba got{1, 1, 1, 1};
  CHECK(c.get(&got) == Result::Ok);
  CHECK(got.r == 0 && got.g == 0 && got.b == 0 && got.a == 0); // before any set
  Rgba set{0.25f, 0.5f, 0.75f, 1.0f};
  CHECK(c.set(&set) == Result::Ok);
  CHECK(c.get(&got) == Result::Ok);
  CHECK(got.r == 0.25f && got.g == 0.5f && got.b == 0.75f && got.a == 1.0f);
  CHECK(c.set(nullptr) == Result::InvalidArg);
  CHECK(c.get(nullptr) == Result::InvalidArg);
  CHECK(c.get(&got) == Result::Ok && got.g == 0.5f); // a failed set changes nothing
}

void
TestSourceSize() {
  SourceSize s;
  uint32_t w = 0, h = 0;
  CHECK(s.get(&w, &h, 1280, 720) == Result::Ok);
  CHECK_EQ(w, 1280u);
  CHECK_EQ(h, 720u); // the buffers' size before any set
  CHECK(s.set(640, 360, 1280, 720) == Result::Ok);
  CHECK(s.get(&w, &h, 1280, 720) == Result::Ok);
  CHECK_EQ(w, 640u);
  CHECK_EQ(h, 360u);
  CHECK(s.set(1280, 720, 1280, 720) == Result::Ok); // the whole buffer
  CHECK(s.set(0, 720, 1280, 720) == Result::InvalidArg);
  CHECK(s.set(1280, 0, 1280, 720) == Result::InvalidArg);
  CHECK(s.set(1281, 720, 1280, 720) == Result::InvalidArg);
  CHECK(s.set(1280, 721, 1280, 720) == Result::InvalidArg);
  CHECK(s.get(&w, &h, 1280, 720) == Result::Ok);
  CHECK_EQ(w, 1280u); // the failed sets kept the last good one
  // one pointer may be null, not both
  w = 0;
  CHECK(s.get(&w, nullptr, 1280, 720) == Result::Ok);
  CHECK_EQ(w, 1280u);
  CHECK(s.get(nullptr, nullptr, 1280, 720) == Result::InvalidArg);
  // ResizeBuffers: the new buffers' size again
  s.set(320, 200, 1280, 720);
  s.reset();
  CHECK(s.get(&w, &h, 1920, 1080) == Result::Ok);
  CHECK_EQ(w, 1920u);
  CHECK_EQ(h, 1080u);
}

void
TestResidency() {
  int a = 0, b = 0;
  const void *resources[] = {&a, &b};
  uint32_t residency[2] = {0, 0};
  CHECK(QueryResidency(resources, residency, 2) == Result::Ok);
  CHECK_EQ(residency[0], kResidencyFullyResident);
  CHECK_EQ(residency[1], kResidencyFullyResident);
  CHECK(QueryResidency(nullptr, residency, 2) == Result::InvalidArg);
  CHECK(QueryResidency(resources, nullptr, 2) == Result::InvalidArg);
  CHECK(QueryResidency(nullptr, nullptr, 0) == Result::Ok);
}

} // namespace

int
main() {
  TestRotation();
  TestBackgroundColor();
  TestSourceSize();
  TestResidency();
  return TestSummary("test_swapchain_state");
}
