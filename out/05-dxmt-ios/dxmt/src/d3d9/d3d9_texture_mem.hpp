// SPDX-License-Identifier: LGPL-2.1-or-later
// From David Acevedo's DXMT fork (github.com/dacevedo12/dxmt), tag v0.4-d3d9,
// commit e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b. LGPL-2.1-or-later: see COPYING.LIB
// and LICENSE.LGPL at the top of this tree. Imported for MYIOSDECK (spec 07).
#pragma once

#include <cstdint>

namespace dxmt {

// Pure arithmetic behind GetAvailableTextureMem, split out so the host tier can
// pin the megabyte-granularity mask and the never-negative contract without a
// Metal device. The device supplies the two inputs it alone can read (the raw
// recommendedMaxWorkingSetSize budget and hasUnifiedMemory), the once-resolved
// DXMT_MAX_VRAM_MB ceiling, and the running DEFAULT-pool allocation total; this
// applies the fixed transform:
//   - halve the budget on unified memory (the dxgi/d3d11 mirror);
//   - clamp below 4 GB (0xfff00000) so the value fits UINT and the running
//     counter stays visible instead of saturating a multi-GB working set;
//   - apply the configurable ceiling (0 = none);
//   - subtract what the app has allocated, clamped at 0 (DXVK's max(mem, 0);
//     wined3d underflows here, a bug we do not port);
//   - mask to megabyte granularity (DXVK's & 0xfff00000, "as per spec").
// The mask guarantees (result & 0xFFFFF) == 0, and result is always >= 0.
inline uint32_t
available_texture_mem_bytes(uint64_t raw_budget, bool unified, uint64_t config_cap_bytes, int64_t used) {
  uint64_t bytes = raw_budget;
  if (unified)
    bytes /= 2;

  constexpr uint64_t kReportCap = 0xfff00000ull;
  if (bytes > kReportCap)
    bytes = kReportCap;

  if (config_cap_bytes && bytes > config_cap_bytes)
    bytes = config_cap_bytes;

  if (used > 0) {
    uint64_t u = static_cast<uint64_t>(used);
    bytes = u >= bytes ? 0 : bytes - u;
  }

  return static_cast<uint32_t>(bytes) & 0xfff00000u;
}

} // namespace dxmt
