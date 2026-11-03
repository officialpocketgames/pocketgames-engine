// SPDX-License-Identifier: LGPL-2.1-or-later
// From steamac's DXMT patch 0012 (SteveTanSaMa, tests/regression/shader_bitcode_cleanup.cpp of
// SteveTanSaMa/DXMT 4689d1bb), fitted for MYIOSDECK spec 05 clarification 10 (this tree's CHECK
// helpers instead of assert).
//
// CompiledBitcode (dxmt/src/d3d11/d3d11_shader_bitcode.hpp) releases the compiled bitcode on
// every way out of the shader compile task: the MTLLibrary failing, success, an exception
// (a cache write that throws), and not at all for a null result.

#include "test_common.hpp"

#include "d3d11_shader_bitcode.hpp"

#include <stdexcept>
#include <type_traits>

namespace {
unsigned g_destroyed = 0;
} // namespace

extern "C" AIRCONV_API void
SM50DestroyBitcode(sm50_bitcode_t bitcode) {
  ++g_destroyed;
  delete static_cast<int *>(bitcode);
}

namespace {

void
Compile(bool library_failed, bool throw_on_cache_write) {
  auto bitcode = new int(42);
  dxmt::CompiledBitcode owned(bitcode);
  if (library_failed)
    return;
  if (throw_on_cache_write)
    throw std::runtime_error("cache write");
}

void
TestEveryPath() {
  static_assert(!std::is_copy_constructible_v<dxmt::CompiledBitcode>);
  Compile(true, false);
  CHECK_EQ(g_destroyed, 1);
  Compile(false, false);
  CHECK_EQ(g_destroyed, 2);
  try {
    Compile(false, true);
  } catch (const std::runtime_error &) {
  }
  CHECK_EQ(g_destroyed, 3);
  { dxmt::CompiledBitcode empty(nullptr); }
  CHECK_EQ(g_destroyed, 3);
}

} // namespace

int
main() {
  TestEveryPath();
  return TestSummary("test_shader_bitcode");
}
