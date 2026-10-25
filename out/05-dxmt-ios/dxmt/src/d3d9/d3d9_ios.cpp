// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 Part C: reading the switches once, the module-wide mirror budget and
// the per-device log line (C16).

#include "d3d9_ios.hpp"

#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"

#include <string>

namespace dxmt {

namespace {

bool
Switch(const char *name, bool default_on) {
  const std::string value = env::getEnvVar(name);
  return d9ios::ParseSwitch(value.c_str(), default_on);
}

D9IosConfig
ReadConfig() {
  D9IosConfig c{};
  c.upload_commit_bytes = d9ios::UploadCommitThresholdBytes(env::getEnvVar("DXMT_D9_UPLOAD_COMMIT_MB").c_str());
  c.batch_budget = Switch("PG_D3D9_BATCH_BUDGET", true);
  c.scene_batch = Switch("DXMT_D9_SCENE_BATCH", true);
  // Either variable set to 0 turns batched readbacks off.
  c.batch_readback = Switch("DXMT_D9_BATCH_READBACK", true) && Switch("PG_D3D9_MIRROR_BATCH", true);
  c.query_adaptive = Switch("DXMT_D9_QUERY_ADAPTIVE", true);
  // The virtual monitor exists only when the app set PG_SCREEN_W (spec 02).
  c.virtual_mode = Switch("DXMT_D9_VIRTUAL_MODE", true) && !env::getEnvVar("PG_SCREEN_W").empty();
  c.canonical_blend = Switch("DXMT_D9_CANONICAL_BLEND", true);
  c.binding_prune = Switch("DXMT_D9_BINDING_PRUNE", true);
  c.shader_prune = Switch("DXMT_D9_SHADER_PRUNE", true);
  c.aniso_limit = d9ios::ParseAnisotropyLimit(env::getEnvVar("DXMT_D9_ANISO_LIMIT").c_str());
  c.small_mirror_cache = Switch("DXMT_D9_SMALL_MIRROR_CACHE", true);
  return c;
}

} // namespace

const D9IosConfig &
D9Ios() {
  static const D9IosConfig config = ReadConfig();
  return config;
}

d9ios::MirrorCacheBudget &
D9MirrorCacheBudget() {
  static d9ios::MirrorCacheBudget budget;
  return budget;
}

void
LogD9IosDevice(bool bc_emulated) {
  const D9IosConfig &c = D9Ios();
  Logger::info(str::format(
      "d3d9 device: bc_decode=", bc_emulated ? 1 : 0, " upload_commit_mb=", c.upload_commit_bytes >> 20,
      " batch_budget=", c.batch_budget ? 1 : 0, " scene_batch=", c.scene_batch ? 1 : 0,
      " batch_readback=", c.batch_readback ? 1 : 0, " query_adaptive=", c.query_adaptive ? 1 : 0,
      " virtual_mode=", c.virtual_mode ? 1 : 0, " canonical_blend=", c.canonical_blend ? 1 : 0,
      " binding_prune=", c.binding_prune ? 1 : 0, " shader_prune=", c.shader_prune ? 1 : 0,
      " aniso_limit=", c.aniso_limit, " small_mirror_cache=", c.small_mirror_cache ? 1 : 0
  ));
}

} // namespace dxmt
