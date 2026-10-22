// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 Part C: block-compression decoder. See dxmt_bc_decode.hpp.
//
// Sources: Microsoft, "Texture Block Compression in Direct3D 11", "BC7 Format"
// and "BC7 Format Mode Reference" (learn.microsoft.com) for the algorithms and
// the interpolation weights. The BC7 partition and anchor tables are not on
// Microsoft's pages; they are the tables Table.P2, Table.P3, Table.A2,
// Table.A3a and Table.A3b of the Khronos ARB_texture_compression_bptc
// specification (the same format under the name BPTC). See PROVENANCE.md.

#include "dxmt_bc_decode.hpp"

#include <cstring>
#include <utility>

namespace dxmt::bc {

namespace {

inline uint32_t
Load16(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
}

inline uint32_t
Load32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t
Load64(const uint8_t *p) {
  return uint64_t(Load32(p)) | (uint64_t(Load32(p + 4)) << 32);
}

/* ---- BC1-BC3 colour block ------------------------------------------------ */

/* RGB565 to RGB888 by bit replication (equal to rounding v * 255 / 31 or 63). */
inline void
Expand565(uint32_t c, uint8_t out[4]) {
  uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
  out[0] = uint8_t((r << 3) | (r >> 2));
  out[1] = uint8_t((g << 2) | (g >> 4));
  out[2] = uint8_t((b << 3) | (b >> 2));
  out[3] = 255;
}

/* `three_colour_allowed` is true only for BC1: with color0 <= color1 the block
   is in three-colour mode (the third colour is the 1:1 blend, index 3 is
   transparent black). BC2 and BC3 colour blocks always use four colours. */
void
DecodeColourBlock(const uint8_t *block, bool three_colour_allowed, uint8_t texels[16 * 4]) {
  uint32_t c0 = Load16(block), c1 = Load16(block + 2);
  uint32_t indices = Load32(block + 4);
  uint8_t palette[4][4];
  Expand565(c0, palette[0]);
  Expand565(c1, palette[1]);
  if (!three_colour_allowed || c0 > c1) {
    for (int ch = 0; ch < 3; ch++) {
      palette[2][ch] = uint8_t((2 * palette[0][ch] + palette[1][ch] + 1) / 3);
      palette[3][ch] = uint8_t((palette[0][ch] + 2 * palette[1][ch] + 1) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int ch = 0; ch < 3; ch++)
      palette[2][ch] = uint8_t((palette[0][ch] + palette[1][ch] + 1) / 2);
    palette[2][3] = 255;
    palette[3][0] = palette[3][1] = palette[3][2] = palette[3][3] = 0;
  }
  for (int t = 0; t < 16; t++)
    std::memcpy(texels + t * 4, palette[(indices >> (2 * t)) & 3], 4);
}

/* ---- BC3 alpha, BC4, BC5 channel blocks ------------------------------------ */

/* Unsigned: eight values when e0 > e1, otherwise six interpolated values plus
   0 and 255. Writes one byte per texel with the given stride. */
void
DecodeUnormChannel(const uint8_t *block, uint8_t *out, size_t stride) {
  uint32_t e0 = block[0], e1 = block[1];
  uint64_t indices = Load64(block) >> 16; /* 48 bits, 3 per texel */
  uint8_t palette[8];
  palette[0] = uint8_t(e0);
  palette[1] = uint8_t(e1);
  if (e0 > e1) {
    for (uint32_t i = 1; i <= 6; i++)
      palette[i + 1] = uint8_t(((7 - i) * e0 + i * e1 + 3) / 7);
  } else {
    for (uint32_t i = 1; i <= 4; i++)
      palette[i + 1] = uint8_t(((5 - i) * e0 + i * e1 + 2) / 5);
    palette[6] = 0;
    palette[7] = 255;
  }
  for (int t = 0; t < 16; t++)
    out[t * stride] = palette[(indices >> (3 * t)) & 7];
}

/* Division rounded to nearest, halves away from zero. */
inline int
DivRound(int num, int den) {
  return (num >= 0 ? num + den / 2 : num - den / 2) / den;
}

/* Signed: an endpoint of -128 reads as -127 first; the mode is then chosen by
   comparing those values (spec 05 clarification 2: e0 = -127, e1 = -128 is
   the six-value mode). Six-value mode adds -127 (-1.0) and +127 (+1.0). */
void
DecodeSnormChannel(const uint8_t *block, uint8_t *out, size_t stride) {
  int raw0 = int8_t(block[0]), raw1 = int8_t(block[1]);
  int e0 = raw0 == -128 ? -127 : raw0;
  int e1 = raw1 == -128 ? -127 : raw1;
  uint64_t indices = Load64(block) >> 16;
  int palette[8];
  palette[0] = e0;
  palette[1] = e1;
  if (e0 > e1) {
    for (int i = 1; i <= 6; i++)
      palette[i + 1] = DivRound((7 - i) * e0 + i * e1, 7);
  } else {
    for (int i = 1; i <= 4; i++)
      palette[i + 1] = DivRound((5 - i) * e0 + i * e1, 5);
    palette[6] = -127;
    palette[7] = 127;
  }
  for (int t = 0; t < 16; t++)
    out[t * stride] = uint8_t(int8_t(palette[(indices >> (3 * t)) & 7]));
}

/* ---- BC7 ------------------------------------------------------------------ */

struct ModeInfo {
  uint8_t subsets;         // NS
  uint8_t partition_bits;  // PB
  uint8_t rotation_bits;   // RB
  uint8_t index_sel_bits;  // ISB
  uint8_t colour_bits;     // CB
  uint8_t alpha_bits;      // AB
  uint8_t endpoint_pbits;  // EPB (one P-bit per endpoint)
  uint8_t shared_pbits;    // SPB (one P-bit per subset)
  uint8_t index_bits;      // IB
  uint8_t index2_bits;     // IB2
};

constexpr ModeInfo kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, //
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, //
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, //
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, //
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, //
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, //
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, //
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}, //
};

constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

constexpr uint8_t kPartition2[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1}, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1}, {0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0}, {0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1}, {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0}, {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0}, {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1}, {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1},
    {0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0}, {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0}, {0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1},
    {0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1},
    {0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1}, {0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1},
    {0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0}, {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1},
};

constexpr uint8_t kPartition3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0}, {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1}, {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2}, {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};

/* Anchor index of the second subset (two subsets). */
constexpr uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, //
    15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  //
    15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,  //
    6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15, //
};

/* Anchor index of the second subset (three subsets). */
constexpr uint8_t kAnchor3a[64] = {
    3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  //
    3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15, //
    8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15, //
    3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3,  //
};

/* Anchor index of the third subset (three subsets). */
constexpr uint8_t kAnchor3b[64] = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  //
    15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  //
    15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,  //
    15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,  //
};

/* Reads a 128-bit block LSB first. Every read is at most 8 bits. */
class BitReader {
public:
  explicit BitReader(const uint8_t *block) : lo_(Load64(block)), hi_(Load64(block + 8)) {}

  uint32_t
  read(unsigned count) {
    if (count == 0)
      return 0;
    uint64_t v;
    if (pos_ >= 64)
      v = hi_ >> (pos_ - 64);
    else if (pos_ + count <= 64)
      v = lo_ >> pos_;
    else
      v = (lo_ >> pos_) | (hi_ << (64 - pos_));
    pos_ += count;
    return uint32_t(v & ((1u << count) - 1));
  }

  void
  skip(unsigned count) {
    pos_ += count;
  }

private:
  uint64_t lo_, hi_;
  unsigned pos_ = 0;
};

inline uint8_t
Interpolate(uint32_t e0, uint32_t e1, uint32_t index, uint32_t index_bits) {
  uint32_t w = index_bits == 2 ? kWeights2[index] : index_bits == 3 ? kWeights3[index] : kWeights4[index];
  return uint8_t(((64 - w) * e0 + w * e1 + 32) >> 6);
}

void
DecodeBC7Block(const uint8_t *block, uint8_t texels[16 * 4]) {
  unsigned mode = 0;
  while (mode < 8 && !(block[0] & (1u << mode)))
    mode++;
  if (mode == 8) {
    /* Reserved encoding: a block of all zeroes. */
    std::memset(texels, 0, 16 * 4);
    return;
  }
  const ModeInfo &m = kModes[mode];
  BitReader bits(block);
  bits.skip(mode + 1);

  uint32_t partition = bits.read(m.partition_bits);
  uint32_t rotation = bits.read(m.rotation_bits);
  uint32_t index_sel = bits.read(m.index_sel_bits);

  const unsigned endpoints = 2u * m.subsets;
  uint32_t ep[6][4] = {};
  for (unsigned ch = 0; ch < 3; ch++)
    for (unsigned e = 0; e < endpoints; e++)
      ep[e][ch] = bits.read(m.colour_bits);
  if (m.alpha_bits)
    for (unsigned e = 0; e < endpoints; e++)
      ep[e][3] = bits.read(m.alpha_bits);

  unsigned colour_prec = m.colour_bits, alpha_prec = m.alpha_bits;
  if (m.endpoint_pbits) {
    for (unsigned e = 0; e < endpoints; e++) {
      uint32_t p = bits.read(1);
      for (unsigned ch = 0; ch < 3; ch++)
        ep[e][ch] = (ep[e][ch] << 1) | p;
      if (m.alpha_bits)
        ep[e][3] = (ep[e][3] << 1) | p;
    }
    colour_prec++;
    if (m.alpha_bits)
      alpha_prec++;
  }
  if (m.shared_pbits) {
    for (unsigned s = 0; s < m.subsets; s++) {
      uint32_t p = bits.read(1);
      for (unsigned e = 2 * s; e < 2 * s + 2; e++)
        for (unsigned ch = 0; ch < 3; ch++)
          ep[e][ch] = (ep[e][ch] << 1) | p;
    }
    colour_prec++;
  }

  /* Move each component's MSB to bit 7 and replicate the top bits below. */
  for (unsigned e = 0; e < endpoints; e++) {
    for (unsigned ch = 0; ch < 3; ch++) {
      uint32_t v = ep[e][ch] << (8 - colour_prec);
      ep[e][ch] = (v | (v >> colour_prec)) & 0xff;
    }
    if (m.alpha_bits) {
      uint32_t v = ep[e][3] << (8 - alpha_prec);
      ep[e][3] = (v | (v >> alpha_prec)) & 0xff;
    } else {
      ep[e][3] = 255;
    }
  }

  uint8_t subset_of[16];
  uint8_t anchor[3] = {0, 0, 0};
  for (unsigned t = 0; t < 16; t++)
    subset_of[t] = m.subsets == 1   ? 0
                   : m.subsets == 2 ? kPartition2[partition][t]
                                    : kPartition3[partition][t];
  if (m.subsets == 2) {
    anchor[1] = kAnchor2[partition];
  } else if (m.subsets == 3) {
    anchor[1] = kAnchor3a[partition];
    anchor[2] = kAnchor3b[partition];
  }

  /* The anchor index of each subset has one bit fewer (its top bit is 0). */
  uint8_t index1[16], index2[16] = {};
  for (unsigned t = 0; t < 16; t++) {
    bool is_anchor = t == anchor[subset_of[t]];
    index1[t] = uint8_t(bits.read(m.index_bits - (is_anchor ? 1 : 0)));
  }
  if (m.index2_bits) {
    for (unsigned t = 0; t < 16; t++)
      index2[t] = uint8_t(bits.read(m.index2_bits - (t == 0 ? 1 : 0)));
  }

  for (unsigned t = 0; t < 16; t++) {
    const uint32_t *e0 = ep[2 * subset_of[t]];
    const uint32_t *e1 = ep[2 * subset_of[t] + 1];
    uint32_t colour_index = index1[t], colour_bits = m.index_bits;
    uint32_t alpha_index = index1[t], alpha_bits = m.index_bits;
    if (m.index2_bits) {
      if (index_sel == 0) {
        alpha_index = index2[t];
        alpha_bits = m.index2_bits;
      } else {
        colour_index = index2[t];
        colour_bits = m.index2_bits;
      }
    }
    uint8_t *out = texels + t * 4;
    for (unsigned ch = 0; ch < 3; ch++)
      out[ch] = Interpolate(e0[ch], e1[ch], colour_index, colour_bits);
    out[3] = Interpolate(e0[3], e1[3], alpha_index, alpha_bits);

    switch (rotation) {
    case 1: std::swap(out[0], out[3]); break;
    case 2: std::swap(out[1], out[3]); break;
    case 3: std::swap(out[2], out[3]); break;
    default: break;
    }
  }
}

} // namespace

void
DecodeBlock(Format format, const uint8_t *block, uint8_t *texels) {
  switch (format) {
  case Format::BC1:
    DecodeColourBlock(block, true, texels);
    break;
  case Format::BC2: {
    DecodeColourBlock(block + 8, false, texels);
    uint64_t alpha = Load64(block);
    for (int t = 0; t < 16; t++)
      texels[t * 4 + 3] = uint8_t(((alpha >> (4 * t)) & 0xf) * 17);
    break;
  }
  case Format::BC3:
    DecodeColourBlock(block + 8, false, texels);
    DecodeUnormChannel(block, texels + 3, 4);
    break;
  case Format::BC4_UNORM:
    DecodeUnormChannel(block, texels, 1);
    break;
  case Format::BC4_SNORM:
    DecodeSnormChannel(block, texels, 1);
    break;
  case Format::BC5_UNORM:
    DecodeUnormChannel(block, texels, 2);
    DecodeUnormChannel(block + 8, texels + 1, 2);
    break;
  case Format::BC5_SNORM:
    DecodeSnormChannel(block, texels, 2);
    DecodeSnormChannel(block + 8, texels + 1, 2);
    break;
  case Format::BC7:
    DecodeBC7Block(block, texels);
    break;
  }
}

bool
DecodeSubresource(
    Format format, const void *src, size_t src_row_pitch, void *dst, size_t dst_row_pitch, uint32_t width,
    uint32_t height
) {
  if (width == 0 || height == 0)
    return true;
  if (!src || !dst)
    return false;
  const uint32_t bpp = DecodedBytesPerTexel(format);
  const uint32_t block_bytes = BlockBytes(format);
  const uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
  if (src_row_pitch < size_t(blocks_x) * block_bytes)
    return false;
  if (dst_row_pitch == 0)
    dst_row_pitch = size_t(width) * bpp;

  const uint8_t *src_bytes = static_cast<const uint8_t *>(src);
  uint8_t *dst_bytes = static_cast<uint8_t *>(dst);
  uint8_t texels[16 * 4];
  for (uint32_t by = 0; by < blocks_y; by++) {
    const uint8_t *block_row = src_bytes + size_t(by) * src_row_pitch;
    const uint32_t rows = height - by * 4 < 4 ? height - by * 4 : 4;
    for (uint32_t bx = 0; bx < blocks_x; bx++) {
      DecodeBlock(format, block_row + size_t(bx) * block_bytes, texels);
      const uint32_t cols = width - bx * 4 < 4 ? width - bx * 4 : 4;
      for (uint32_t y = 0; y < rows; y++) {
        uint8_t *out = dst_bytes + size_t(by * 4 + y) * dst_row_pitch + size_t(bx * 4) * bpp;
        std::memcpy(out, texels + (y * 4) * bpp, size_t(cols) * bpp);
      }
    }
  }
  return true;
}

bool
DecodeVolume(
    Format format, const void *src, size_t src_row_pitch, size_t src_depth_pitch, void *dst, size_t dst_row_pitch,
    size_t dst_depth_pitch, uint32_t width, uint32_t height, uint32_t depth
) {
  if (width == 0 || height == 0 || depth == 0)
    return true;
  if (!src || !dst)
    return false;
  const uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
  if (src_row_pitch < size_t(blocks_x) * BlockBytes(format))
    return false;
  if (depth > 1 && src_depth_pitch < size_t(blocks_y) * src_row_pitch)
    return false;
  if (dst_row_pitch == 0)
    dst_row_pitch = size_t(width) * DecodedBytesPerTexel(format);
  if (dst_depth_pitch == 0)
    dst_depth_pitch = dst_row_pitch * height;
  for (uint32_t z = 0; z < depth; z++) {
    DecodeSubresource(
        format, static_cast<const uint8_t *>(src) + size_t(z) * src_depth_pitch, src_row_pitch,
        static_cast<uint8_t *>(dst) + size_t(z) * dst_depth_pitch, dst_row_pitch, width, height
    );
  }
  return true;
}

void
FillBC6HGrey(void *dst, size_t dst_row_pitch, uint32_t width, uint32_t height) {
  /* IEEE half floats: 0.25 = 0x3400, 1.0 = 0x3C00 (little-endian RGBA16F). */
  static const uint8_t grey[8] = {0x00, 0x34, 0x00, 0x34, 0x00, 0x34, 0x00, 0x3c};
  if (!dst)
    return;
  if (dst_row_pitch == 0)
    dst_row_pitch = size_t(width) * 8;
  uint8_t *rows = static_cast<uint8_t *>(dst);
  for (uint32_t y = 0; y < height; y++)
    for (uint32_t x = 0; x < width; x++)
      std::memcpy(rows + size_t(y) * dst_row_pitch + size_t(x) * 8, grey, 8);
}

} // namespace dxmt::bc
