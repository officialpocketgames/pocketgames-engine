// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 Part C: the switches of the Direct3D 9 frontend's iOS behaviour,
// each read once from the environment (d3d9_ios.cpp), and the module-wide
// small-mirror budget. The rules they feed are in d3d9_ios_policy.hpp.

#pragma once

#include "d3d9_ios_policy.hpp"

#include <cstdint>

namespace dxmt {

struct D9IosConfig {
  uint64_t upload_commit_bytes; // C3: DXMT_D9_UPLOAD_COMMIT_MB (default 64 MB; 0 never)
  bool batch_budget;            // C4: PG_D3D9_BATCH_BUDGET (default on)
  bool scene_batch;             // C5: DXMT_D9_SCENE_BATCH (default on)
  bool batch_readback;          // C6: DXMT_D9_BATCH_READBACK and PG_D3D9_MIRROR_BATCH (both default on)
  bool query_adaptive;          // C7: DXMT_D9_QUERY_ADAPTIVE (default on)
  bool virtual_mode;            // C8: DXMT_D9_VIRTUAL_MODE (default on) and PG_SCREEN_W set
  bool canonical_blend;         // C11: DXMT_D9_CANONICAL_BLEND (default on)
  bool binding_prune;           // C12: DXMT_D9_BINDING_PRUNE (default on)
  bool shader_prune;            // C12: DXMT_D9_SHADER_PRUNE (default on)
  uint32_t aniso_limit;         // C13: DXMT_D9_ANISO_LIMIT (1/2/4/8; 0 no cap)
  bool small_mirror_cache;      // C15: DXMT_D9_SMALL_MIRROR_CACHE (default on)
};

/* The switches, read from the environment the first time this is called. */
const D9IosConfig &D9Ios();

/* C15: bytes of small managed mirrors kept, shared by every device of the
   module. */
d9ios::MirrorCacheBudget &D9MirrorCacheBudget();

/* C16: the one line a device logs at creation. */
void LogD9IosDevice(bool bc_emulated);

} // namespace dxmt
