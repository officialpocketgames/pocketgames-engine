// SPDX-License-Identifier: LGPL-2.1-or-later
// From David Acevedo's DXMT fork (github.com/dacevedo12/dxmt), tag v0.4-d3d9,
// commit e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b. LGPL-2.1-or-later: see COPYING.LIB
// and LICENSE.LGPL at the top of this tree. Imported for MYIOSDECK (spec 07).
#pragma once

#include "d3d9.h"

namespace dxmt {

// IDirect3DResource9::SetPriority, shared by the resource types that carry a
// managed-eviction priority (2D/cube/volume textures, vertex/index buffers).
// wined3d_resource_set_priority honors the value only for D3DPOOL_MANAGED
// resources: every other pool ignores the call and reports 0, so GetPriority
// keeps reading the stored field (which stays 0 because the set was a no-op).
// Surfaces ignore priority unconditionally and return 0 at their own call
// sites (d3d9_surface_SetPriority does the same).
inline DWORD
D3D9SetResourcePriority(D3DPOOL pool, DWORD &priority, DWORD newPriority) {
  if (pool != D3DPOOL_MANAGED)
    return 0;
  DWORD prev = priority;
  priority = newPriority;
  return prev;
}

} // namespace dxmt
