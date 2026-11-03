// SPDX-License-Identifier: LGPL-2.1-or-later
// From steamac's DXMT patch 0012 (SteveTanSaMa, "fix(d3d11): release compiled bitcode on all
// shader failure paths", SteveTanSaMa/DXMT 4689d1bb), added for MYIOSDECK spec 05
// clarification 10 (see LICENSE-NOTES.md).
#pragma once

#include "airconv_public.h"

namespace dxmt {

// sm50_bitcode_t is a pointer on 64-bit and a pointer wrapper on WoW64.
// Keep the owner in that representation rather than converting to void*.
class CompiledBitcode {
public:
  explicit CompiledBitcode(sm50_bitcode_t bitcode) : bitcode_(bitcode) {}
  CompiledBitcode(const CompiledBitcode &) = delete;
  CompiledBitcode &operator=(const CompiledBitcode &) = delete;
  ~CompiledBitcode() {
    if (bitcode_)
      SM50DestroyBitcode(bitcode_);
  }

private:
  sm50_bitcode_t bitcode_;
};

} // namespace dxmt
