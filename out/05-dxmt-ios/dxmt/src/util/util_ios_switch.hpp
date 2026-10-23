// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05: switches are read from the environment: unset or empty = the
// default given, "0" = off, anything else = on. Callers read each switch once
// (keep the result in a static).

#pragma once

#include "util_env.hpp"
#include <string>

namespace dxmt::env {

inline bool
readSwitch(const char *name, bool default_on) {
  std::string value = getEnvVar(name);
  if (value.empty())
    return default_on;
  return value != "0";
}

} // namespace dxmt::env
