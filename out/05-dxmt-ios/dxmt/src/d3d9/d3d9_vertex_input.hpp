// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 6: Direct3D 9 vertex elements.
//   - to_mtl_attr_format: D3DDECLTYPE -> the format the DXSO / fixed-function vertex fetch decodes
//     (moved here from d3d9_device.cpp; the Direct3D 9 path fetches every vertex in the shader,
//     air_operations.cpp pull_vec4_from_addr, so a dxmt-private value is fine where Metal has no
//     matching format: UDEC3 and DEC3N).
//   - D3D9Decode: what Direct3D 9 defines each type to read as (Microsoft's D3DDECLTYPE
//     documentation), with the default components (0, 0, 0, 1).
//   - ShaderFetchModel: a CPU model of what the shader fetch produces for each format value, line
//     for line with air_operations.cpp (Metal's unpack conversions as the Metal Shading Language
//     specification gives them). The host test checks that decoding through the table and the
//     model gives D3D9Decode's values for every type; the shader itself only runs on the GPU.
//   - The declaration log: `[d3d9] vertex declaration <n>: <stream>:<offset> <type> <usage><index>, ...`,
//     each distinct declaration once per process, at most 64.
// Pure: no Metal or device types.

#pragma once

#include "d3d9.h"
#include "airconv_public.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace dxmt {

/* ---- the mapping ---- */

/* MTLVertexFormat values (Apple's Metal headers) the table uses */
namespace mtl_vertex_format {
constexpr uint32_t UChar4 = 3, UChar4Normalized = 9, Short2 = 16, Short4 = 18, UShort2Normalized = 19,
                   UShort4Normalized = 21, Short2Normalized = 22, Short4Normalized = 24, Half2 = 25, Half4 = 27,
                   Float = 28, Float2 = 29, Float3 = 30, Float4 = 31, Int1010102Normalized = 40,
                   UChar4Normalized_BGRA = 42;
} // namespace mtl_vertex_format

/* D3DDECLTYPE -> fetch format. D3DCOLOR's 0xAARRGGBB word is UChar4Normalized_BGRA. */
inline uint32_t
to_mtl_attr_format(uint32_t type) {
  using namespace mtl_vertex_format;
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return Float;
  case D3DDECLTYPE_FLOAT2:
    return Float2;
  case D3DDECLTYPE_FLOAT3:
    return Float3;
  case D3DDECLTYPE_FLOAT4:
    return Float4;
  case D3DDECLTYPE_D3DCOLOR:
    return UChar4Normalized_BGRA;
  case D3DDECLTYPE_UBYTE4:
    return UChar4; // integers, presented as floats (5 -> 5.0)
  case D3DDECLTYPE_SHORT2:
    return Short2;
  case D3DDECLTYPE_SHORT4:
    return Short4;
  case D3DDECLTYPE_UBYTE4N:
    return UChar4Normalized;
  case D3DDECLTYPE_SHORT2N:
    return Short2Normalized;
  case D3DDECLTYPE_SHORT4N:
    return Short4Normalized;
  case D3DDECLTYPE_USHORT2N:
    return UShort2Normalized;
  case D3DDECLTYPE_USHORT4N:
    return UShort4Normalized;
  case D3DDECLTYPE_FLOAT16_2:
    return Half2;
  case D3DDECLTYPE_FLOAT16_4:
    return Half4;
  case D3DDECLTYPE_DEC3N:
    // MYIOSDECK spec 07 clarification 6: signed normalized 10:10:10 with w = 1. Metal's
    // Int1010102Normalized has the same bit order for x, y and z, but its fourth lane is the
    // 2-bit field, where D3D9 has none: w came out as that field (0 for most data), not 1. The
    // fetch decodes the raw word and sets w = 1 (DXSO_ATTR_FORMAT_DEC3N).
    return DXSO_ATTR_FORMAT_DEC3N;
  case D3DDECLTYPE_UDEC3:
    // unsigned 10:10:10, not normalized, w = 1; Metal has no such format
    return DXSO_ATTR_FORMAT_UDEC3;
  default:
    return 0; // invalid; CreateVertexDeclaration refuses such a type
  }
}

/* ---- streams ---- */

/* The bytes a stream bound at `offset` into a buffer of `size` bytes holds from its start, which
   the fetch clamps its reads to (its last whole vertex starts `stride` bytes before that). An
   offset at or past the end leaves none. */
inline uint32_t
StreamBytesAfterOffset(uint64_t size, uint64_t offset) {
  return offset < size ? uint32_t(std::min<uint64_t>(size - offset, UINT32_MAX)) : 0u;
}

/* The byte offset of vertex `index` the fetch reads (dxso_compile.cpp / ffp_compile.cpp): stride *
   index, clamped to the start of the last whole vertex the stream's `length` bytes hold; a zero
   stride reads vertex 0 for every vertex. */
inline uint32_t
FetchVertexBase(uint32_t stride, uint32_t index, uint32_t length) {
  const uint32_t last = length >= stride ? length - stride : 0u;
  const uint32_t base = stride * index;
  return base < last ? base : last;
}

/* The vertex index an element reads (dxso_compile.cpp): per-vertex elements the vertex id; an
   instanced stream (SetStreamSourceFreq D3DSTREAMSOURCE_INSTANCEDATA | n, step_rate n >= 1) the
   instance / n. */
inline uint32_t
FetchIndex(bool per_instance, uint32_t step_rate, uint32_t vertex_id, uint32_t instance_id, uint32_t base_instance) {
  if (!per_instance)
    return vertex_id;
  return step_rate ? base_instance + instance_id / step_rate : base_instance;
}

/* ---- half floats ---- */

/* IEEE 754 binary16 -> float, with infinities, NaNs and denormals */
inline float
HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  const uint32_t exp = (h >> 10) & 0x1f;
  const uint32_t mant = h & 0x3ff;
  float value;
  if (exp == 0) {
    value = std::ldexp(float(mant), -24); // zero or denormal: mant * 2^-24
  } else if (exp == 31) {
    value = mant ? std::nanf("") : INFINITY;
  } else {
    value = std::ldexp(float(mant | 0x400), int(exp) - 25);
  }
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  bits |= sign;
  std::memcpy(&value, &bits, 4);
  return value;
}

/* ---- reading little-endian fields ---- */

inline uint16_t
ReadU16(const uint8_t *p) {
  return uint16_t(p[0] | (p[1] << 8));
}

inline uint32_t
ReadU32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline float
ReadF32(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}

/* signed normalized, as Direct3D 10 and Metal define it: c / (2^(n-1) - 1), clamped to -1 */
inline float
Snorm(int32_t c, uint32_t bits) {
  const float max = float((1u << (bits - 1)) - 1u);
  return std::max(float(c) / max, -1.0f);
}

inline float
Unorm(uint32_t c, uint32_t bits) {
  return float(c) / float((1u << bits) - 1u);
}

inline int32_t
SignExtend(uint32_t value, uint32_t bits) {
  const uint32_t m = 1u << (bits - 1);
  return int32_t((value ^ m) - m);
}

/* ---- Direct3D 9's definition ---- */

/* What a vertex element of `type` at `p` reads as in a vertex shader input register. Missing
   components are (0, 0, 0, 1). */
inline void
D3D9Decode(uint32_t type, const uint8_t *p, float out[4]) {
  out[0] = 0, out[1] = 0, out[2] = 0, out[3] = 1;
  switch (type) {
  case D3DDECLTYPE_FLOAT4:
    out[3] = ReadF32(p + 12);
    [[fallthrough]];
  case D3DDECLTYPE_FLOAT3:
    out[2] = ReadF32(p + 8);
    [[fallthrough]];
  case D3DDECLTYPE_FLOAT2:
    out[1] = ReadF32(p + 4);
    [[fallthrough]];
  case D3DDECLTYPE_FLOAT1:
    out[0] = ReadF32(p);
    break;
  case D3DDECLTYPE_D3DCOLOR: { // 0xAARRGGBB -> (R, G, B, A)
    const uint32_t c = ReadU32(p);
    out[0] = Unorm((c >> 16) & 0xff, 8), out[1] = Unorm((c >> 8) & 0xff, 8);
    out[2] = Unorm(c & 0xff, 8), out[3] = Unorm(c >> 24, 8);
    break;
  }
  case D3DDECLTYPE_UBYTE4:
    for (int i = 0; i < 4; i++)
      out[i] = float(p[i]);
    break;
  case D3DDECLTYPE_UBYTE4N:
    for (int i = 0; i < 4; i++)
      out[i] = Unorm(p[i], 8);
    break;
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT4:
    for (int i = 0; i < (type == D3DDECLTYPE_SHORT2 ? 2 : 4); i++)
      out[i] = float(int16_t(ReadU16(p + 2 * i)));
    break;
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_SHORT4N:
    for (int i = 0; i < (type == D3DDECLTYPE_SHORT2N ? 2 : 4); i++)
      out[i] = Snorm(int16_t(ReadU16(p + 2 * i)), 16);
    break;
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_USHORT4N:
    for (int i = 0; i < (type == D3DDECLTYPE_USHORT2N ? 2 : 4); i++)
      out[i] = Unorm(ReadU16(p + 2 * i), 16);
    break;
  case D3DDECLTYPE_UDEC3: { // (x, y, z, 1), unsigned 10-bit integers
    const uint32_t w = ReadU32(p);
    out[0] = float(w & 0x3ff), out[1] = float((w >> 10) & 0x3ff), out[2] = float((w >> 20) & 0x3ff);
    break;
  }
  case D3DDECLTYPE_DEC3N: { // (x, y, z, 1), signed normalized 10-bit
    const uint32_t w = ReadU32(p);
    out[0] = Snorm(SignExtend(w & 0x3ff, 10), 10);
    out[1] = Snorm(SignExtend((w >> 10) & 0x3ff, 10), 10);
    out[2] = Snorm(SignExtend((w >> 20) & 0x3ff, 10), 10);
    break;
  }
  case D3DDECLTYPE_FLOAT16_2:
  case D3DDECLTYPE_FLOAT16_4:
    for (int i = 0; i < (type == D3DDECLTYPE_FLOAT16_2 ? 2 : 4); i++)
      out[i] = HalfToFloat(ReadU16(p + 2 * i));
    break;
  default:
    out[3] = 0; // invalid: the fetch zero-fills
    break;
  }
}

/* ---- the shader fetch, modelled ---- */

/* What air_operations.cpp pull_vec4_from_addr_checked (and the integer to float conversion after
   it in dxso_compile.cpp / ffp_compile.cpp) yields for `format` at `p`. */
inline void
ShaderFetchModel(uint32_t format, const uint8_t *p, float out[4]) {
  using namespace mtl_vertex_format;
  out[0] = 0, out[1] = 0, out[2] = 0, out[3] = 1;
  switch (format) {
  case Float4:
    out[3] = ReadF32(p + 12);
    [[fallthrough]];
  case Float3:
    out[2] = ReadF32(p + 8);
    [[fallthrough]];
  case Float2:
    out[1] = ReadF32(p + 4);
    [[fallthrough]];
  case Float:
    out[0] = ReadF32(p);
    break;
  case UChar4Normalized:
  case UChar4Normalized_BGRA: { // air.unpack.unorm4x8 of the word: x is the low byte
    const uint32_t c = ReadU32(p);
    float v[4];
    for (int i = 0; i < 4; i++)
      v[i] = Unorm((c >> (8 * i)) & 0xff, 8);
    const int order_bgra[4] = {2, 1, 0, 3}, order[4] = {0, 1, 2, 3};
    const int *o = format == UChar4Normalized_BGRA ? order_bgra : order;
    for (int i = 0; i < 4; i++)
      out[i] = v[o[i]];
    break;
  }
  case UChar4: // zero-extended, then SIToFP
    for (int i = 0; i < 4; i++)
      out[i] = float(int32_t(p[i]));
    break;
  case Short2:
  case Short4: // sign-extended, then SIToFP; Short2 pads (0, 1)
    for (int i = 0; i < (format == Short2 ? 2 : 4); i++)
      out[i] = float(int32_t(int16_t(ReadU16(p + 2 * i))));
    break;
  case Short2Normalized:
  case Short4Normalized: // air.unpack.snorm2x16 per word
    for (int i = 0; i < (format == Short2Normalized ? 2 : 4); i++)
      out[i] = Snorm(int16_t(ReadU16(p + 2 * i)), 16);
    break;
  case UShort2Normalized:
  case UShort4Normalized: // air.unpack.unorm2x16 per word
    for (int i = 0; i < (format == UShort2Normalized ? 2 : 4); i++)
      out[i] = Unorm(ReadU16(p + 2 * i), 16);
    break;
  case Half2:
  case Half4:
    for (int i = 0; i < (format == Half2 ? 2 : 4); i++)
      out[i] = HalfToFloat(ReadU16(p + 2 * i));
    break;
  case Int1010102Normalized:
  case DXSO_ATTR_FORMAT_DEC3N: { // air.unpack.snorm.rgb10a2: w is the 2-bit field
    const uint32_t w = ReadU32(p);
    out[0] = Snorm(SignExtend(w & 0x3ff, 10), 10);
    out[1] = Snorm(SignExtend((w >> 10) & 0x3ff, 10), 10);
    out[2] = Snorm(SignExtend((w >> 20) & 0x3ff, 10), 10);
    out[3] = Snorm(SignExtend(w >> 30, 2), 2);
    if (format == DXSO_ATTR_FORMAT_DEC3N)
      out[3] = 1; // spec 07 clarification 6
    break;
  }
  case DXSO_ATTR_FORMAT_UDEC3: { // shifts, w = 1, then SIToFP
    const uint32_t w = ReadU32(p);
    out[0] = float(int32_t(w & 0x3ff)), out[1] = float(int32_t((w >> 10) & 0x3ff));
    out[2] = float(int32_t((w >> 20) & 0x3ff));
    break;
  }
  default:
    out[3] = 0; // no value: the fetch zero-fills
    break;
  }
}

/* ---- the declaration log ---- */

inline const char *
DeclTypeName(uint32_t type) {
  static const char *const names[] = {"FLOAT1",  "FLOAT2",  "FLOAT3",   "FLOAT4",   "D3DCOLOR", "UBYTE4",
                                      "SHORT2",  "SHORT4",  "UBYTE4N",  "SHORT2N",  "SHORT4N",  "USHORT2N",
                                      "USHORT4N", "UDEC3",  "DEC3N",    "FLOAT16_2", "FLOAT16_4", "UNUSED"};
  return type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

inline const char *
DeclUsageName(uint32_t usage) {
  static const char *const names[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",    "PSIZE",
                                      "TEXCOORD", "TANGENT",     "BINORMAL",     "TESSFACTOR", "POSITIONT",
                                      "COLOR",    "FOG",         "DEPTH",        "SAMPLE"};
  return usage < sizeof(names) / sizeof(names[0]) ? names[usage] : "?";
}

/* `[d3d9] vertex declaration <n>: <stream>:<offset> <type> <usage><index>, ...` (elements up to the
   terminator) */
inline std::string
DeclarationLine(unsigned number, const D3DVERTEXELEMENT9 *elements, size_t count) {
  std::string line = "[d3d9] vertex declaration " + std::to_string(number) + ":";
  bool first = true;
  for (size_t i = 0; i < count && elements[i].Stream != 0xFF; i++) {
    const D3DVERTEXELEMENT9 &e = elements[i];
    line += first ? " " : ", ";
    first = false;
    line += std::to_string(e.Stream) + ":" + std::to_string(e.Offset) + " " + DeclTypeName(e.Type) + " " +
            DeclUsageName(e.Usage) + std::to_string(e.UsageIndex);
  }
  if (first)
    line += " (empty)";
  return line;
}

/* Spec 07 clarification 9: the vertex shader input register a declaration element feeds: the
   first input dcl with the element's usage and usage index (the caller matches POSITIONT as
   POSITION), -1 for none. Usage and index are compared as numbers, whatever they are: an element
   at TEXCOORD6 feeds dcl_texcoord6 as COLOR0 feeds dcl_color0, and its format comes from its type
   alone (to_mtl_attr_format). `dcls` is DxsoShaderMetadata::dcls (a template so this header
   needn't include the decoder). */
template <typename BoundDcls>
inline int
VsInputRegister(const BoundDcls &dcls, uint32_t usage, uint32_t usage_index) {
  for (const auto &d : dcls) {
    using RegisterType = decltype(d.bound_to.type);
    if (d.bound_to.type == RegisterType::Input && static_cast<uint32_t>(d.dcl.usage) == usage &&
        d.dcl.usage_index == usage_index)
      return static_cast<int>(d.bound_to.num);
  }
  return -1;
}

/* Each distinct declaration once, at most kMaxLogged: the number to log it under, or 0 when it
   was logged already or the limit is reached. Compares the elements up to the terminator.
   Spec 07 clarification 9: number() gives any declaration its number, the same for every
   declaration with the same elements (the frame trace's draw lines print it); the first
   kMaxLogged distinct ones are the logged ones, and numbering stops at kMaxNumbered (0 after). */
class DeclarationLog {
public:
  static constexpr unsigned kMaxLogged = 64;
  static constexpr unsigned kMaxNumbered = 4096;

  struct Numbered {
    unsigned number; // 0: past kMaxNumbered
    bool log;        // first seen, and within kMaxLogged: log it now
  };

  Numbered
  number(const D3DVERTEXELEMENT9 *elements, size_t count) {
    std::vector<uint8_t> key;
    for (size_t i = 0; i < count && elements[i].Stream != 0xFF; i++) {
      const D3DVERTEXELEMENT9 &e = elements[i];
      const uint8_t bytes[8] = {uint8_t(e.Stream), uint8_t(e.Stream >> 8), uint8_t(e.Offset), uint8_t(e.Offset >> 8),
                                e.Type,            e.Method,             e.Usage,           e.UsageIndex};
      key.insert(key.end(), bytes, bytes + 8);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find(seen_.begin(), seen_.end(), key);
    if (found != seen_.end())
      return {unsigned(found - seen_.begin()) + 1, false};
    if (seen_.size() == kMaxNumbered)
      return {0, false};
    seen_.push_back(std::move(key));
    return {unsigned(seen_.size()), seen_.size() <= kMaxLogged};
  }

  unsigned
  take(const D3DVERTEXELEMENT9 *elements, size_t count) {
    Numbered numbered = number(elements, count);
    return numbered.log ? numbered.number : 0;
  }

private:
  std::mutex mutex_;
  std::vector<std::vector<uint8_t>> seen_;
};

} // namespace dxmt
