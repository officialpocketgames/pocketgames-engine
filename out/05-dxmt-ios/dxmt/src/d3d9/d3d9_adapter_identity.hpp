// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 8: the adapter identity Direct3D 9 reports (GetAdapterIdentifier, also
// through IDirect3D9Ex). Some engines pick their shader package from the vendor and device IDs and
// don't know Apple's; an unrecognised GPU is commonly answered with an NVIDIA identity. So, by
// default:
//   VendorId 0x10DE, DeviceId 0x0402, "NVIDIA GeForce 8600 GT", a driver version of that card
//   (9.18.13.4144, NVIDIA's 341.44), and the DeviceIdentifier GUID as before.
// PG_D3D9_ADAPTER=<vendor hex>:<device hex>:<description> overrides the IDs and the description;
// PG_D3D9_ADAPTER=metal gives back the Apple identity (0x106B, 1, the Metal device's name, the
// newest-driver version). A value that is neither is ignored, with a line, and the default used.
// Nothing else follows from the identity: caps, formats and memory stay what the device supports.
// Direct3D 10/11 and DXGI keep their own.
//
// Pure (no Windows types), so tests/test_d3d9_adapter.cpp builds it as it is.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dxmt::d9adapter {

enum class Source { Default, Env, Metal };

constexpr uint32_t kNvidiaVendor = 0x10DE;
constexpr uint32_t kGeForce8600GT = 0x0402;
constexpr const char *kGeForce8600GTName = "NVIDIA GeForce 8600 GT";
constexpr uint32_t kAppleVendor = 0x106B;
constexpr uint32_t kMetalDevice = 1;
/* D3DADAPTER_IDENTIFIER9::Description holds MAX_DEVICE_IDENTIFIER_STRING (512) bytes */
constexpr size_t kMaxDescription = 511;

struct Identity {
  uint32_t vendor = kNvidiaVendor;
  uint32_t device = kGeForce8600GT;
  std::string description = kGeForce8600GTName;
  Source source = Source::Default;
  /* DriverVersion: product, version, sub-version, build (HIWORD/LOWORD of HighPart, then of
     LowPart); all 0 means "the newest possible driver" (INT64_MAX), as before */
  uint16_t driver[4] = {9, 18, 13, 4144};
};

inline const char *
SourceName(Source source) {
  switch (source) {
  case Source::Default: return "default";
  case Source::Env: return "env";
  case Source::Metal: return "metal";
  }
  return "default";
}

/* 1 to 4 hexadecimal digits, with or without 0x */
inline bool
ParseHex16(const std::string &text, uint32_t *out) {
  size_t start = (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? 2 : 0;
  size_t digits = text.size() - start;
  if (digits < 1 || digits > 4)
    return false;
  uint32_t value = 0;
  for (size_t i = start; i < text.size(); i++) {
    const char c = text[i];
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = uint32_t(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = uint32_t(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = uint32_t(c - 'A' + 10);
    else
      return false;
    value = (value << 4) | d;
  }
  *out = value;
  return true;
}

inline bool
IsMetal(const std::string &value) {
  if (value.size() != 5)
    return false;
  const char *m = "metal";
  for (size_t i = 0; i < 5; i++) {
    char c = value[i];
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
    if (c != m[i])
      return false;
  }
  return true;
}

/* The identity for PG_D3D9_ADAPTER's value (null or empty: unset). `metal_name` is the Metal
   device's name, for `metal`. *malformed says a value was given and ignored. */
inline Identity
Choose(const char *value, const std::string &metal_name, bool *malformed) {
  *malformed = false;
  Identity id;
  if (!value || !*value)
    return id;
  const std::string text = value;
  if (IsMetal(text)) {
    id.vendor = kAppleVendor;
    id.device = kMetalDevice;
    id.description = metal_name.substr(0, kMaxDescription);
    id.source = Source::Metal;
    for (auto &part : id.driver)
      part = 0;
    return id;
  }
  const size_t first = text.find(':');
  const size_t second = first == std::string::npos ? std::string::npos : text.find(':', first + 1);
  uint32_t vendor = 0, device = 0;
  if (second == std::string::npos || !ParseHex16(text.substr(0, first), &vendor) ||
      !ParseHex16(text.substr(first + 1, second - first - 1), &device) || second + 1 >= text.size()) {
    *malformed = true;
    return id;
  }
  id.vendor = vendor;
  id.device = device;
  id.description = text.substr(second + 1, kMaxDescription); // the rest, colons and all
  id.source = Source::Env;
  for (auto &part : id.driver) // an identity of the program's choosing: the newest driver
    part = 0;
  return id;
}

/* DriverVersion.QuadPart */
inline int64_t
DriverVersionQuad(const Identity &id) {
  if (!id.driver[0] && !id.driver[1] && !id.driver[2] && !id.driver[3])
    return INT64_MAX;
  const uint64_t high = (uint64_t(id.driver[0]) << 16) | id.driver[1];
  const uint64_t low = (uint64_t(id.driver[2]) << 16) | id.driver[3];
  return int64_t((high << 32) | low);
}

/* "[d3d9] adapter identity 10de:0402 "NVIDIA GeForce 8600 GT" (default)" */
inline std::string
Line(const Identity &id) {
  char ids[32];
  std::snprintf(ids, sizeof(ids), "%04x:%04x", id.vendor, id.device);
  return std::string("[d3d9] adapter identity ") + ids + " \"" + id.description + "\" (" + SourceName(id.source) + ")";
}

inline std::string
MalformedLine(const char *value) {
  return std::string("[d3d9] PG_D3D9_ADAPTER \"") + (value ? value : "") +
         "\" is neither <vendor hex>:<device hex>:<description> nor metal; ignored";
}

} // namespace dxmt::d9adapter
