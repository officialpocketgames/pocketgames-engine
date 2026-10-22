// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Memory the Windows half allocates itself and hands to Metal with
// newBufferWithBytesNoCopy: ("placed" memory). Metal wants it page-aligned,
// and iOS pages are 16 KB (DXMT_PAGE_SIZE is 4 KB, the Windows page). Under
// DXMT_IOS placed memory is aligned to 16 KB and its length rounded up to a
// multiple of 16 KB. Spec 06 B makes every CPU-visible buffer of the i386
// build placed; the 64-bit build uses the same rule for its placed memory.

#pragma once

#include <cstddef>

namespace dxmt {

#ifdef DXMT_IOS
constexpr size_t kPlacedMemoryAlignment = 16384;
#else
constexpr size_t kPlacedMemoryAlignment = DXMT_PAGE_SIZE;
#endif

/* The length to allocate (and give Metal) for `length` bytes of placed memory. */
constexpr size_t
PlacedMemoryLength(size_t length) {
#ifdef DXMT_IOS
  return (length + kPlacedMemoryAlignment - 1) & ~(kPlacedMemoryAlignment - 1);
#else
  return length;
#endif
}

} // namespace dxmt
