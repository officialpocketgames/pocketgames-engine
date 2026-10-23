// SPDX-License-Identifier: LGPL-2.1-or-later
// From David Acevedo's DXMT fork (github.com/dacevedo12/dxmt), tag v0.4-d3d9,
// commit e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b. LGPL-2.1-or-later: see COPYING.LIB
// and LICENSE.LGPL at the top of this tree. Imported for MYIOSDECK (spec 07).
/*
 * Private diagnostic interface for MTLD3D9Device (tests only, not visible to apps).
 * Lifetime: pointer is BORROWED; callers must NOT AddRef/Release.
 * Does not inherit IUnknown; lifetime governed by IDirect3DDevice9 refcount.
 */
#pragma once

#include "d3d9.h"

namespace dxmt {

struct IDxmtDiag9 {
  virtual UINT STDMETHODCALLTYPE GetLosableResourceCount() = 0;
};

// {D2C7B12B-D9D9-4D9D-8AAA-BBCCDDEE0001}
//
// Private dxmt diag UUID. Random fourth-version GUID; no app or
// upstream tooling enumerates QI tables for this value.
inline constexpr GUID IID_IDxmtDiag9 = {0xD2C7B12B, 0xD9D9, 0x4D9D, {0x8A, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00, 0x01}};

} // namespace dxmt
