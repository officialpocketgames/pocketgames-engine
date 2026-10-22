// SPDX-License-Identifier: LGPL-2.1-or-later
// From David Acevedo's DXMT fork (github.com/dacevedo12/dxmt), tag v0.4-d3d9,
// commit e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b. LGPL-2.1-or-later: see COPYING.LIB
// and LICENSE.LGPL at the top of this tree. Imported for MYIOSDECK (spec 07).
#pragma once

#include "d3d9.h"

namespace dxmt {

// Initialize 256-entry D3DRS render-state array with D3D9 defaults.
// enableAutoDepthStencil controls D3DRS_ZENABLE default (wined3d ref).
void init_default_render_states(DWORD (&rs)[256], bool enableAutoDepthStencil);

// Seed `count` sampler stages with the D3D9 power-on sampler-state
// defaults (reference: wined3d stateblock.c init_default_sampler_states).
void init_default_sampler_states(DWORD (*samp)[D3DSAMP_DMAPOFFSET + 1], unsigned int count);

// Zero, then seed, `stages` texture-stage-state blocks with the D3D9
// power-on defaults (reference: Wine d3d9 test_texture_stage_states).
void init_default_texture_stage_states(DWORD (*tss)[D3DTSS_CONSTANT + 1], unsigned int stages);

} // namespace dxmt
