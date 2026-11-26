// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 6: Direct3D 9 vertex elements (dxmt/src/d3d9/d3d9_vertex_input.hpp).
//   - Every D3DDECLTYPE against hand-computed values, with D3D9's default components;
//     UDEC3 and DEC3N edge values; FLOAT16 infinities, NaN and denormals.
//   - Decoding through to_mtl_attr_format and the model of the shader fetch gives D3D9's values
//     for every type and every test pattern (the model follows air_operations.cpp; the
//     generated shader itself only runs on the GPU). DEC3N through Metal's Int1010102Normalized,
//     as before this change, gives w from the 2-bit field instead of 1.
//   - The stream rules: the length after an offset, the clamp to the last whole vertex, stride 0,
//     the instanced index.
//   - The declaration line, once per distinct declaration, at most 64.

#include "test_common.hpp"

#include "d3d9_vertex_input.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace dxmt;

/* little-endian bytes of the given values */
struct Bytes {
  uint8_t b[16] = {};
  Bytes &u32(unsigned at, uint32_t v) {
    for (int i = 0; i < 4; i++)
      b[at + i] = uint8_t(v >> (8 * i));
    return *this;
  }
  Bytes &u16(unsigned at, uint16_t v) {
    b[at] = uint8_t(v), b[at + 1] = uint8_t(v >> 8);
    return *this;
  }
  Bytes &f32(unsigned at, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return u32(at, v);
  }
};

bool
Same(float a, float b) {
  if (std::isnan(a) || std::isnan(b))
    return std::isnan(a) && std::isnan(b);
  if (a == 0 && b == 0)
    return std::signbit(a) == std::signbit(b);
  return a == b || std::fabs(a - b) <= 1e-6f * std::max(std::fabs(a), std::fabs(b));
}

bool
Is(uint32_t type, const Bytes &bytes, float x, float y, float z, float w) {
  float out[4];
  D3D9Decode(type, bytes.b, out);
  const float want[4] = {x, y, z, w};
  bool ok = true;
  for (int i = 0; i < 4; i++)
    ok &= Same(out[i], want[i]);
  if (!ok)
    std::printf("  %s: got (%g, %g, %g, %g), want (%g, %g, %g, %g)\n", DeclTypeName(type), out[0], out[1], out[2],
                out[3], x, y, z, w);
  return ok;
}

void
TestTypes() {
  // floats and the default components
  Bytes f;
  f.f32(0, 1.5f).f32(4, -2.0f).f32(8, 3.25f).f32(12, 0.5f);
  CHECK(Is(D3DDECLTYPE_FLOAT1, f, 1.5f, 0, 0, 1));
  CHECK(Is(D3DDECLTYPE_FLOAT2, f, 1.5f, -2.0f, 0, 1));
  CHECK(Is(D3DDECLTYPE_FLOAT3, f, 1.5f, -2.0f, 3.25f, 1));
  CHECK(Is(D3DDECLTYPE_FLOAT4, f, 1.5f, -2.0f, 3.25f, 0.5f));
  // D3DCOLOR 0xAARRGGBB -> (R, G, B, A)
  CHECK(Is(D3DDECLTYPE_D3DCOLOR, Bytes().u32(0, 0x80FF4020u), 1.0f, 64 / 255.0f, 32 / 255.0f, 128 / 255.0f));
  // UBYTE4: integers as floats
  CHECK(Is(D3DDECLTYPE_UBYTE4, Bytes().u32(0, 0x80FF0005u), 5, 0, 255, 128));
  CHECK(Is(D3DDECLTYPE_UBYTE4N, Bytes().u32(0, 0x80FF0005u), 5 / 255.0f, 0, 1, 128 / 255.0f));
  // SHORT2 / SHORT4: integers as floats, not normalized
  CHECK(Is(D3DDECLTYPE_SHORT2, Bytes().u16(0, 0x8000).u16(2, 0x7fff), -32768, 32767, 0, 1));
  CHECK(Is(D3DDECLTYPE_SHORT4, Bytes().u16(0, 0xffff).u16(2, 100).u16(4, 0x8000).u16(6, 7), -1, 100, -32768, 7));
  // SHORT2N / SHORT4N: c / 32767, -32768 clamped to -1
  CHECK(Is(D3DDECLTYPE_SHORT2N, Bytes().u16(0, 0x8000).u16(2, 0x7fff), -1, 1, 0, 1));
  CHECK(Is(D3DDECLTYPE_SHORT4N, Bytes().u16(0, 0x8001).u16(2, 0).u16(4, 16384).u16(6, 0xffff), -1, 0,
           16384 / 32767.0f, -1 / 32767.0f));
  // USHORT2N / USHORT4N: c / 65535
  CHECK(Is(D3DDECLTYPE_USHORT2N, Bytes().u16(0, 0xffff).u16(2, 0), 1, 0, 0, 1));
  CHECK(Is(D3DDECLTYPE_USHORT4N, Bytes().u16(0, 32768).u16(2, 1).u16(4, 0xffff).u16(6, 0), 32768 / 65535.0f,
           1 / 65535.0f, 1, 0));
  // UDEC3: (x, y, z, 1), unsigned 10-bit integers; the top two bits ignored
  CHECK(Is(D3DDECLTYPE_UDEC3, Bytes().u32(0, 0x3fffffffu), 1023, 1023, 1023, 1));
  CHECK(Is(D3DDECLTYPE_UDEC3, Bytes().u32(0, 0xc0000000u), 0, 0, 0, 1));
  CHECK(Is(D3DDECLTYPE_UDEC3, Bytes().u32(0, 1u | (2u << 10) | (1022u << 20)), 1, 2, 1022, 1));
  // DEC3N: (x, y, z, 1), signed normalized 10-bit: 511 -> 1, -512 and -511 -> -1, -1 -> -1/511
  CHECK(Is(D3DDECLTYPE_DEC3N, Bytes().u32(0, 0x1ffu | (0x200u << 10) | (0x201u << 20)), 1, -1, -1, 1));
  CHECK(Is(D3DDECLTYPE_DEC3N, Bytes().u32(0, 0x3ffu | (0u << 10) | (256u << 20) | 0xc0000000u), -1 / 511.0f, 0,
           256 / 511.0f, 1));
  CHECK(Is(D3DDECLTYPE_DEC3N, Bytes().u32(0, 0), 0, 0, 0, 1));
  // FLOAT16: infinities, NaN, denormals, the largest and smallest normals, -0
  CHECK(Is(D3DDECLTYPE_FLOAT16_2, Bytes().u16(0, 0x7c00).u16(2, 0xfc00), INFINITY, -INFINITY, 0, 1));
  CHECK(Is(D3DDECLTYPE_FLOAT16_4, Bytes().u16(0, 0x0001).u16(2, 0x03ff).u16(4, 0x0400).u16(6, 0x7bff),
           std::ldexp(1.0f, -24), std::ldexp(1023.0f, -24), std::ldexp(1.0f, -14), 65504.0f));
  CHECK(Is(D3DDECLTYPE_FLOAT16_4, Bytes().u16(0, 0x3c00).u16(2, 0xc000).u16(4, 0x8000).u16(6, 0x7e00), 1, -2, -0.0f,
           NAN));
}

/* Every test pattern, every type: the table and the modelled fetch give D3D9's values */
void
TestFetchMatchesD3D9() {
  std::vector<Bytes> patterns;
  const uint32_t words[] = {0u,          0xffffffffu, 0x3fffffffu, 0xc0000000u, 0x80FF4020u,
                            0x1ff00201u, 0x7c00fc00u, 0x00013c00u, 0x12345678u, 0x7fff8000u};
  for (uint32_t a : words)
    for (uint32_t b : {0u, 0x3f800000u, 0x7e000400u, 0xffff0001u})
      patterns.push_back(Bytes().u32(0, a).u32(4, b).u32(8, a ^ b).u32(12, b * 3 + 1));
  for (uint32_t type = D3DDECLTYPE_FLOAT1; type <= D3DDECLTYPE_FLOAT16_4; type++) {
    const uint32_t format = to_mtl_attr_format(type);
    CHECK(format != 0);
    bool all = true;
    for (const Bytes &p : patterns) {
      float want[4], got[4];
      D3D9Decode(type, p.b, want);
      ShaderFetchModel(format, p.b, got);
      for (int i = 0; i < 4; i++)
        if (!Same(got[i], want[i])) {
          if (all)
            std::printf("  %s: lane %d got %g, want %g\n", DeclTypeName(type), i, got[i], want[i]);
          all = false;
        }
    }
    CHECK(all);
  }
  // the fault this clarification fixes: DEC3N through Metal's Int1010102Normalized reads w from
  // the 2-bit field (0 for most data), not D3D9's 1
  float got[4];
  ShaderFetchModel(mtl_vertex_format::Int1010102Normalized, Bytes().u32(0, 0x1ffu).b, got);
  CHECK(got[3] == 0);
  CHECK(to_mtl_attr_format(D3DDECLTYPE_DEC3N) == DXSO_ATTR_FORMAT_DEC3N);
  CHECK(to_mtl_attr_format(D3DDECLTYPE_UDEC3) == DXSO_ATTR_FORMAT_UDEC3);
  CHECK(to_mtl_attr_format(D3DDECLTYPE_UNUSED) == 0);
}

void
TestStreams() {
  // the bytes from the stream's start: the buffer less the offset
  CHECK_EQ(StreamBytesAfterOffset(1000, 0), 1000u);
  CHECK_EQ(StreamBytesAfterOffset(1000, 100), 900u);
  CHECK_EQ(StreamBytesAfterOffset(100, 100), 0u);
  CHECK_EQ(StreamBytesAfterOffset(100, 200), 0u);
  // a buffer of 4 vertices of 16 bytes: the fourth is the last whole one
  CHECK_EQ(FetchVertexBase(16, 3, 64), 48u);
  CHECK_EQ(FetchVertexBase(16, 9, 64), 48u);
  // with a 100-byte offset into a 164-byte buffer the stream holds the same 4 vertices: with the
  // whole size (the fault), vertex 9 read 144 bytes past the stream's start, past the buffer
  CHECK_EQ(FetchVertexBase(16, 9, StreamBytesAfterOffset(164, 100)), 48u);
  CHECK_EQ(FetchVertexBase(16, 9, 164), 144u);
  // stride 0: every vertex reads the first
  CHECK_EQ(FetchVertexBase(0, 0, 64), 0u);
  CHECK_EQ(FetchVertexBase(0, 12345, 64), 0u);
  // a stream shorter than one vertex reads its start
  CHECK_EQ(FetchVertexBase(32, 5, 16), 0u);
  // SetStreamSourceFreq: per-vertex streams use the vertex id; INSTANCEDATA | n the instance / n
  CHECK_EQ(FetchIndex(false, 0, 77, 5, 0), 77u);
  CHECK_EQ(FetchIndex(true, 1, 77, 5, 0), 5u);
  CHECK_EQ(FetchIndex(true, 2, 77, 5, 0), 2u);
  CHECK_EQ(FetchIndex(true, 3, 77, 9, 0), 3u);
}

void
TestDeclarationLog() {
  const D3DVERTEXELEMENT9 decl[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_DEC3N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
      {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      {1, 0, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
      {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0},
  };
  CHECK(DeclarationLine(3, decl, 5) ==
        "[d3d9] vertex declaration 3: 0:0 FLOAT3 POSITION0, 0:12 DEC3N NORMAL0, 0:16 D3DCOLOR COLOR0, 1:0 FLOAT16_2 "
        "TEXCOORD1");
  const D3DVERTEXELEMENT9 empty[] = {{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0}};
  CHECK(DeclarationLine(1, empty, 1) == "[d3d9] vertex declaration 1: (empty)");

  DeclarationLog log;
  CHECK_EQ(log.take(decl, 5), 1u);
  CHECK_EQ(log.take(decl, 5), 0u); // the same again: not logged
  D3DVERTEXELEMENT9 other[5];
  std::memcpy(other, decl, sizeof(decl));
  other[1].Type = D3DDECLTYPE_UDEC3;
  CHECK_EQ(log.take(other, 5), 2u);
  // at most 64 in all
  unsigned taken = 2;
  for (uint16_t offset = 100; offset < 200; offset++) {
    other[0].Offset = offset;
    if (log.take(other, 5))
      taken++;
  }
  CHECK_EQ(taken, DeclarationLog::kMaxLogged);
}

} // namespace

int
main() {
  TestTypes();
  TestFetchMatchesD3D9();
  TestStreams();
  TestDeclarationLog();
  return TestSummary("test_d3d9_vertex_input");
}
