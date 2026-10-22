// SPDX-License-Identifier: LGPL-2.1-or-later
// From David Acevedo's DXMT fork (github.com/dacevedo12/dxmt), tag v0.4-d3d9,
// commit e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b. LGPL-2.1-or-later: see COPYING.LIB
// and LICENSE.LGPL at the top of this tree. Imported for MYIOSDECK (spec 07).
#pragma once

#include "d3d9.h"

#include <cstddef>

namespace dxmt {

// Total length, in DWORDs, of a D3D9 shader bytecode blob: the version
// token through the End token inclusive. Returns 0 for a malformed blob
// (bad version token, or no reachable End). CreateVertexShader /
// CreatePixelShader carry no length, so this delimits the bytecode to
// copy and walk. Kept in its own translation unit so it links against
// only the header-only DXSO decoder, which keeps it host-native testable.
size_t shader_bytecode_dword_count(const DWORD *byte_code);

} // namespace dxmt
