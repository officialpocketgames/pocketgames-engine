// SPDX-License-Identifier: LGPL-2.1-or-later
// The reference cases are from steamac's DXMT patch 0011 (SteveTanSaMa,
// tests/regression/reference_ownership.cpp of SteveTanSaMa/DXMT 8b3a6d08), fitted for MYIOSDECK
// spec 05 clarification 10 (this tree's CHECK helpers instead of assert). The out-error cases
// (Error::out_handle, steamac's DXMT patch 0008) are MYIOSDECK's own.
//
// WMT::Reference (dxmt/src/winemetal/Metal.hpp) with a fake NSObject_retain / NSObject_release
// that count references and flag a retain or release of a freed object: self assignment, a
// borrowed copy of the last owner, moves between two owners of one object, nullptr; and an
// out-error slot reused by a second call releases the first error.

#include "test_common.hpp"

#include "Metal.hpp"

#include <type_traits>
#include <unordered_map>
#include <utility>

namespace {
std::unordered_map<obj_handle_t, unsigned> g_refs;
unsigned g_releases = 0;
unsigned g_misuse = 0; // a retain or release of an object that is gone
} // namespace

extern "C" void
NSObject_retain(obj_handle_t handle) {
  if (!handle)
    return;
  auto iter = g_refs.find(handle);
  // A retain after the final release is a use-after-free, even if handle values match.
  if (iter == g_refs.end()) {
    g_misuse++;
    return;
  }
  ++iter->second;
}

extern "C" void
NSObject_release(obj_handle_t handle) {
  if (!handle)
    return;
  auto iter = g_refs.find(handle);
  if (iter == g_refs.end() || !iter->second) {
    g_misuse++;
    return;
  }
  ++g_releases;
  if (!--iter->second)
    g_refs.erase(iter);
}

namespace {

obj_handle_t
MakeObject() {
  static obj_handle_t next = 1;
  g_refs.emplace(next, 1);
  return next++;
}

void
TestReferenceOwnership() {
  static_assert(std::is_nothrow_move_constructible_v<WMT::Reference<WMT::Object>>);
  static_assert(std::is_nothrow_move_assignable_v<WMT::Reference<WMT::Object>>);
  {
    WMT::Reference<WMT::Object> value(MakeObject());
    auto original = value.handle;
    auto &alias = value;
    value = alias; // self copy-assignment
    CHECK(value.handle == original);
    CHECK_EQ(g_refs.at(original), 1);
    value = WMT::Object{original}; // borrowed copy of the last owner
    CHECK_EQ(g_refs.at(original), 1);
    value = std::move(alias); // self move-assignment
    CHECK(value.handle == original);
    CHECK_EQ(g_refs.at(original), 1);
    WMT::Reference<WMT::Object> copy(value);
    CHECK_EQ(g_refs.at(original), 2);
    value = std::move(copy); // two owners of the same handle
    CHECK(!copy);
    CHECK_EQ(g_refs.at(original), 1);
    WMT::Reference<WMT::Object> moved(std::move(value));
    CHECK(!value);
    CHECK(moved.handle == original);
    WMT::Reference<WMT::Object> replacement(MakeObject());
    auto other = replacement.handle;
    replacement = std::move(moved);
    CHECK(!moved);
    CHECK(!g_refs.contains(other));
    replacement = nullptr;
    CHECK(!replacement);
    CHECK(!g_refs.contains(original));
    WMT::Reference<WMT::Object> empty;
    empty = replacement;
    empty = std::move(replacement);
  }
  CHECK(g_refs.empty());
  CHECK(g_releases > 0);
  CHECK_EQ(g_misuse, 0);
}

void
TestErrorOutHandle() {
  // the unix side hands back a retained (+1) error each time (patch 0008)
  WMT::Reference<WMT::Error> err;
  *err.out_handle() = MakeObject();
  auto first = err.handle;
  CHECK_EQ(g_refs.at(first), 1);
  // a retry through the same Reference drops the first error before the slot is written
  auto *slot = err.out_handle();
  CHECK(!g_refs.contains(first));
  CHECK(*slot == 0);
  *slot = MakeObject();
  auto second = err.handle;
  CHECK_EQ(g_refs.at(second), 1);
  // a call that succeeds writes 0: the second error is released, nothing leaks
  *err.out_handle() = 0;
  CHECK(!g_refs.contains(second));
  CHECK(!err);
  CHECK(g_refs.empty());
  CHECK_EQ(g_misuse, 0);
}

} // namespace

int
main() {
  TestReferenceOwnership();
  TestErrorOutHandle();
  return TestSummary("test_metal_reference");
}
