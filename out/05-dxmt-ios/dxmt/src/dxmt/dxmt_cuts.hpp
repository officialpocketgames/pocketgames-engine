// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 73: the switches of round 17's cuts (items 65-72). Each is read once.
// Unset or empty is its default, "0" off, "1" on; anything else is the default, with one
// warning naming the variable. PG_DXMT_SERIALIZE at level 1 or above turns items 68-72 off
// whatever their variables say. Pure: the host test drives it with its own values; the
// encoding context reads the process's once (CutSettingsOnce, dxmt_context.cpp).

#pragma once

#include <cstdint>
#include <string>

namespace dxmt {

enum class CutSwitch : uint8_t {
  StateFilter,   // 65
  DiscardBound,  // 66
  DeferredRing,  // 67
  MergeCompute,  // 68
  KeepDsv,       // 69
  RoStore,       // 70
  Discard,       // 71
  OverwriteLoad, // 72
  Count
};

struct CutSwitchInfo {
  const char *variable;
  const char *name;
  bool default_on;
  bool serialize_turns_off;
};

inline constexpr CutSwitchInfo kCutSwitches[unsigned(CutSwitch::Count)] = {
    {"DXMT_D11_STATE_FILTER", "state-filter", true, false},
    {"DXMT_D11_DISCARD_BOUND", "discard-bound", true, false},
    {"DXMT_D11_DEFERRED_RING", "deferred-ring", true, false},
    {"DXMT_PASS_MERGE_COMPUTE", "merge-compute", false, true},
    {"DXMT_PASS_KEEP_DSV", "keep-dsv", false, true},
    {"DXMT_PASS_RO_STORE", "ro-store", false, true},
    {"DXMT_PASS_DISCARD", "discard", false, true},
    {"DXMT_PASS_OVERWRITE_LOAD", "overwrite-load", false, true},
};

struct CutSettings {
  bool on[unsigned(CutSwitch::Count)] = {};
  bool serialized[unsigned(CutSwitch::Count)] = {}; // turned off by PG_DXMT_SERIALIZE
  bool ignored[unsigned(CutSwitch::Count)] = {};    // a value that isn't empty, 0 or 1

  bool
  operator[](CutSwitch which) const {
    return on[unsigned(which)];
  }
};

/* `value_of(variable)` gives a variable's value (null when unset). */
template <typename ValueOf>
CutSettings
CutSettingsFrom(ValueOf &&value_of, unsigned serialize_level) {
  CutSettings settings;
  for (unsigned i = 0; i < unsigned(CutSwitch::Count); i++) {
    const CutSwitchInfo &info = kCutSwitches[i];
    const char *value = value_of(info.variable);
    bool on = info.default_on;
    if (value && value[0]) {
      if (value[0] == '0' && !value[1])
        on = false;
      else if (value[0] == '1' && !value[1])
        on = true;
      else
        settings.ignored[i] = true;
    }
    if (info.serialize_turns_off && serialize_level >= 1) {
      on = false;
      settings.serialized[i] = true;
    }
    settings.on[i] = on;
  }
  return settings;
}

/* "dxmt: d3d11 cuts: state-filter on, discard-bound on, deferred-ring on, merge-compute off,
   keep-dsv off, ro-store off, discard off, overwrite-load off" ("off (serialize)" for a switch
   PG_DXMT_SERIALIZE turned off) */
inline std::string
CutSettingsLine(const CutSettings &settings) {
  std::string line = "dxmt: d3d11 cuts:";
  for (unsigned i = 0; i < unsigned(CutSwitch::Count); i++) {
    line += i ? ", " : " ";
    line += kCutSwitches[i].name;
    line += settings.on[i] ? " on" : (settings.serialized[i] ? " off (serialize)" : " off");
  }
  return line;
}

/* The process's settings, read on first use (PG_DXMT_SERIALIZE included), with the warnings
   logged then. */
const CutSettings &CutSettingsOnce();

} // namespace dxmt
