// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 13, the parts of the six upstream DXMT fixes that build without LLVM:
//   - the converted-shader cache's version is 20 (19 for these fixes, 20 for clarification 16's
//     precise min, max and saturate), so every shader cached before them is converted again;
//   - the NaN-to-0 flag travels in SM50_SHADER_COMMON_DATA after the fields that were there
//     before, so the argument's existing fields keep their places.
// check-shader-fixes.sh checks the converter's source for the fixes themselves.

#include "test_common.hpp"

#include "airconv_public.h"
#include "dxmt_shader_cache.hpp"

#include <cstddef>

namespace {

void
TestCacheVersion() {
  static_assert(dxmt::kDXMTShaderCacheVersion == 20, "spec 05 clarification 16 bumps the cache to 20");
  CHECK_EQ(dxmt::kDXMTShaderCacheVersion, 20);
}

void
TestCommonArgumentFlags() {
  CHECK_EQ(SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO, 1);
  CHECK(offsetof(SM50_SHADER_COMMON_DATA, flags) > offsetof(SM50_SHADER_COMMON_DATA, metal_version));
  CHECK_EQ(sizeof(((SM50_SHADER_COMMON_DATA *)nullptr)->flags), 4);

  SM50_SHADER_COMMON_DATA common{};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = SM50_SHADER_METAL_320;
  CHECK_EQ(common.flags, 0); // value-initialised: no NaN-to-0 unless a frontend sets it
  common.flags |= SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
  CHECK((common.flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO) != 0);
}

} // namespace

int
main() {
  TestCacheVersion();
  TestCommonArgumentFlags();
  return TestSummary("test_shader_fixes");
}
