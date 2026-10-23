// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 Part I: unit tests for the BC decoder of Part C
// (dxmt/src/dxmt/dxmt_bc_decode.cpp). Plain C++20, no Windows or Metal types.
//
//   c++ -std=c++20 -I../dxmt/src/dxmt test_bc_decode.cpp ../dxmt/src/dxmt/dxmt_bc_decode.cpp -o test_bc_decode
//
// Every block below is put together field by field, and every expected value
// was worked out by hand from Microsoft's "Texture Block Compression in
// Direct3D 11", "BC7 Format" and "BC7 Format Mode Reference" pages (the working
// is in the comments). Partition and anchor values are those of the Khronos
// ARB_texture_compression_bptc tables. Values were picked so that most results
// are exact; where Microsoft gives only weights (BC1/BC3/BC4 interpolation), the
// expected value is the nearest integer.

#include "test_common.hpp"
#include "dxmt_bc_decode.hpp"

#include <initializer_list>
#include <vector>

using dxmt::bc::DecodeBlock;
using dxmt::bc::DecodeSubresource;
using dxmt::bc::Format;

namespace {

/* Writes a 128-bit BC7 block LSB first, the way the format reads it. */
struct BlockWriter {
  uint8_t bytes[16] = {};
  unsigned pos = 0;

  void
  put(uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; i++, pos++)
      if ((value >> i) & 1)
        bytes[pos / 8] |= uint8_t(1u << (pos % 8));
  }

  void
  put_all(std::initializer_list<uint32_t> values, unsigned count) {
    for (uint32_t v : values)
      put(v, count);
  }
};

const uint8_t *
T(const uint8_t *texels, int index, int bytes_per_texel = 4) {
  return texels + index * bytes_per_texel;
}

/* ---- BC1 ------------------------------------------------------------------ */

void
TestBC1FourColour() {
  // color0 = 0xF800 (red 31) -> (255,0,0); color1 = 0x001F (blue 31) -> (0,0,255).
  // color0 > color1: four colours.
  //   c2 = 2/3 c0 + 1/3 c1 = (170, 0, 85); c3 = 1/3 c0 + 2/3 c1 = (85, 0, 170).
  // Indices 0,1,2,3 repeated: each index byte is 0b11'10'01'00 = 0xE4.
  const uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
  uint8_t out[64];
  DecodeBlock(Format::BC1, block, out);
  for (int row = 0; row < 4; row++) {
    CHECK_RGBA(T(out, row * 4 + 0), 255, 0, 0, 255);
    CHECK_RGBA(T(out, row * 4 + 1), 0, 0, 255, 255);
    CHECK_RGBA(T(out, row * 4 + 2), 170, 0, 85, 255);
    CHECK_RGBA(T(out, row * 4 + 3), 85, 0, 170, 255);
  }
}

void
TestBC1Expansion565AndRounding() {
  // color0 = 0xF800 -> (255,0,0). color1 = r 1, g 32, b 0 = 0x0C00:
  //   r 1 -> 1 * 255 / 31 = 8.2 -> 8;  g 32 -> 32 * 255 / 63 = 129.5 -> 130.
  // c2 = ((2*255 + 8) / 3, 130 / 3, 0) = (172.7, 43.3) -> (173, 43, 0)
  // c3 = ((255 + 2*8) / 3, 2*130 / 3, 0) = (90.3, 86.7) -> (90, 87, 0)
  const uint8_t block[8] = {0x00, 0xF8, 0x00, 0x0C, 0xE4, 0xE4, 0xE4, 0xE4};
  uint8_t out[64];
  DecodeBlock(Format::BC1, block, out);
  CHECK_RGBA(T(out, 0), 255, 0, 0, 255);
  CHECK_RGBA(T(out, 1), 8, 130, 0, 255);
  CHECK_RGBA(T(out, 2), 173, 43, 0, 255);
  CHECK_RGBA(T(out, 3), 90, 87, 0, 255);
}

void
TestBC1ThreeColour() {
  // color0 = 0x0000 (black) <= color1 = 0x8000 (r 16 -> 16 * 255 / 31 = 131.6 -> 132):
  // three colours. c2 = 1/2 (c0 + c1) = (66, 0, 0); index 3 = transparent black.
  const uint8_t block[8] = {0x00, 0x00, 0x00, 0x80, 0xE4, 0xE4, 0xE4, 0xE4};
  uint8_t out[64];
  DecodeBlock(Format::BC1, block, out);
  for (int row = 0; row < 4; row++) {
    CHECK_RGBA(T(out, row * 4 + 0), 0, 0, 0, 255);
    CHECK_RGBA(T(out, row * 4 + 1), 132, 0, 0, 255);
    CHECK_RGBA(T(out, row * 4 + 2), 66, 0, 0, 255);
    CHECK_RGBA(T(out, row * 4 + 3), 0, 0, 0, 0);
  }

  // color0 == color1 is also the three-colour mode.
  const uint8_t equal[8] = {0x00, 0x80, 0x00, 0x80, 0xE4, 0xE4, 0xE4, 0xE4};
  DecodeBlock(Format::BC1, equal, out);
  CHECK_RGBA(T(out, 0), 132, 0, 0, 255);
  CHECK_RGBA(T(out, 2), 132, 0, 0, 255);
  CHECK_RGBA(T(out, 3), 0, 0, 0, 0);
}

/* ---- BC2 ------------------------------------------------------------------ */

void
TestBC2() {
  // Explicit 4-bit alpha, texel t has alpha t: nibbles 0..15, low nibble first.
  // Alpha 4 -> 8 bits: a * 255 / 15 = a * 17.
  // Colour block 0x0000 / 0x8000 has color0 <= color1, but BC2 always uses
  // four colours: c2 = (2*0 + 132) / 3 = 44, c3 = (0 + 2*132) / 3 = 88, and
  // index 3 is NOT transparent black.
  const uint8_t block[16] = {
      0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, // alpha
      0x00, 0x00, 0x00, 0x80, 0xE4, 0xE4, 0xE4, 0xE4, // colour
  };
  uint8_t out[64];
  DecodeBlock(Format::BC2, block, out);
  const uint8_t red[4] = {0, 132, 44, 88};
  for (int t = 0; t < 16; t++)
    CHECK_RGBA(T(out, t), red[t & 3], 0, 0, t * 17);
}

/* ---- BC3 / BC4 / BC5 channel blocks ----------------------------------------- */

// Channel indices 0..7 repeated: index i at bits 3i of the 48-bit field is
// 0o76543210 = 0xFAC688 per 8 texels, little-endian bytes 88 C6 FA.
#define CHANNEL_INDICES 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA

// e0 = 210 > e1 = 0: eight values, (7-i)/7 e0 + i/7 e1 = 180, 150, ..., 30.
const uint8_t kEightValueUnorm[8] = {210, 0, 180, 150, 120, 90, 60, 30};
// e0 = 0 <= e1 = 250: six values, (5-i)/5 e0 + i/5 e1 = 50 .. 200, then 0 and 255.
const uint8_t kSixValueUnorm[8] = {0, 250, 50, 100, 150, 200, 0, 255};

void
TestBC3() {
  const uint8_t block[16] = {
      210, 0, CHANNEL_INDICES,                        // alpha, eight-value mode
      0x00, 0x00, 0x00, 0x80, 0xE4, 0xE4, 0xE4, 0xE4, // colour as in TestBC2
  };
  uint8_t out[64];
  DecodeBlock(Format::BC3, block, out);
  const uint8_t red[4] = {0, 132, 44, 88};
  for (int t = 0; t < 16; t++)
    CHECK_RGBA(T(out, t), red[t & 3], 0, 0, kEightValueUnorm[t & 7]);

  const uint8_t six[16] = {
      0, 250, CHANNEL_INDICES,                        // alpha, six-value mode
      0x00, 0x00, 0x00, 0x80, 0xE4, 0xE4, 0xE4, 0xE4,
  };
  DecodeBlock(Format::BC3, six, out);
  for (int t = 0; t < 16; t++)
    CHECK_RGBA(T(out, t), red[t & 3], 0, 0, kSixValueUnorm[t & 7]);
}

void
TestBC4Unorm() {
  uint8_t out[16];
  const uint8_t eight[8] = {210, 0, CHANNEL_INDICES};
  DecodeBlock(Format::BC4_UNORM, eight, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], kEightValueUnorm[t & 7]);

  const uint8_t six[8] = {0, 250, CHANNEL_INDICES};
  DecodeBlock(Format::BC4_UNORM, six, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], kSixValueUnorm[t & 7]);

  // e0 == e1 is the six-value mode: 100 x 6, then 0 and 255.
  const uint8_t equal[8] = {100, 100, CHANNEL_INDICES};
  const uint8_t expected_equal[8] = {100, 100, 100, 100, 100, 100, 0, 255};
  DecodeBlock(Format::BC4_UNORM, equal, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], expected_equal[t & 7]);
}

uint8_t
S(int v) {
  return uint8_t(int8_t(v));
}

void
TestBC4Snorm() {
  uint8_t out[16];

  // e0 = 70 > e1 = 0: eight values 70, 0, 60, 50, 40, 30, 20, 10.
  const uint8_t eight[8] = {70, 0, CHANNEL_INDICES};
  const int expected_eight[8] = {70, 0, 60, 50, 40, 30, 20, 10};
  DecodeBlock(Format::BC4_SNORM, eight, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], S(expected_eight[t & 7]));

  // e0 = 0 > e1 = -70 (signed compare): eight values 0, -70, -10, ..., -60.
  const uint8_t negative[8] = {0, S(-70), CHANNEL_INDICES};
  const int expected_negative[8] = {0, -70, -10, -20, -30, -40, -50, -60};
  DecodeBlock(Format::BC4_SNORM, negative, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], S(expected_negative[t & 7]));

  // e0 = -50 <= e1 = 50: six values -30, -10, 10, 30, then -1.0 (-127) and +1.0 (127).
  const uint8_t six[8] = {S(-50), 50, CHANNEL_INDICES};
  const int expected_six[8] = {-50, 50, -30, -10, 10, 30, -127, 127};
  DecodeBlock(Format::BC4_SNORM, six, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], S(expected_six[t & 7]));

  // e0 = -128 reads as -127. -128 <= 0: six-value mode with e0 = -127, e1 = 0:
  //   4/5 * -127 = -101.6 -> -102; 3/5 -> -76.2 -> -76; 2/5 -> -50.8 -> -51;
  //   1/5 -> -25.4 -> -25.
  const uint8_t min_endpoint[8] = {0x80, 0, CHANNEL_INDICES};
  const int expected_min[8] = {-127, 0, -102, -76, -51, -25, -127, 127};
  DecodeBlock(Format::BC4_SNORM, min_endpoint, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], S(expected_min[t & 7]));
  CHECK(out[0] != 0x80); // never -128

  // The -128 rule applies before the comparison (spec 05 clarification 2):
  // e0 = -127, e1 = -128 -> -127 and -127, not e0 > e1, so the six-value mode:
  // six times -127, then -127 and +127. (Comparing the stored bytes would pick
  // the eight-value mode, where index 7 is -127 too, not +127.)
  const uint8_t both_min[8] = {S(-127), 0x80, CHANNEL_INDICES};
  const int expected_both_min[8] = {-127, -127, -127, -127, -127, -127, -127, 127};
  DecodeBlock(Format::BC4_SNORM, both_min, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], S(expected_both_min[t & 7]));

  // e0 = 127 > e1 = -128 (-> -127): eight values 127 * (7 - 2i) / 7:
  //   i=1: 90.7 -> 91; i=2: 54.4 -> 54; i=3: 18.1 -> 18; then -18, -54, -91.
  const uint8_t full_range[8] = {127, 0x80, CHANNEL_INDICES};
  const int expected_full[8] = {127, -127, 91, 54, 18, -18, -54, -91};
  DecodeBlock(Format::BC4_SNORM, full_range, out);
  for (int t = 0; t < 16; t++)
    CHECK_EQ(out[t], S(expected_full[t & 7]));
}

void
TestBC5() {
  uint8_t out[32];
  // Red: eight-value unorm block; green: six-value unorm block.
  const uint8_t unorm[16] = {210, 0, CHANNEL_INDICES, 0, 250, CHANNEL_INDICES};
  DecodeBlock(Format::BC5_UNORM, unorm, out);
  for (int t = 0; t < 16; t++) {
    CHECK_EQ(out[t * 2 + 0], kEightValueUnorm[t & 7]);
    CHECK_EQ(out[t * 2 + 1], kSixValueUnorm[t & 7]);
  }

  // Red: 70 / 0 (eight values); green: -50 / 50 (six values).
  const uint8_t snorm[16] = {70, 0, CHANNEL_INDICES, S(-50), 50, CHANNEL_INDICES};
  const int red[8] = {70, 0, 60, 50, 40, 30, 20, 10};
  const int green[8] = {-50, 50, -30, -10, 10, 30, -127, 127};
  DecodeBlock(Format::BC5_SNORM, snorm, out);
  for (int t = 0; t < 16; t++) {
    CHECK_EQ(out[t * 2 + 0], S(red[t & 7]));
    CHECK_EQ(out[t * 2 + 1], S(green[t & 7]));
  }

  // The -128 rule applies to each channel, before the mode is chosen.
  const uint8_t min_green[16] = {70, 0, CHANNEL_INDICES, 0x80, 0, CHANNEL_INDICES};
  DecodeBlock(Format::BC5_SNORM, min_green, out);
  CHECK_EQ(out[1], S(-127));
  CHECK_EQ(out[2 * 2 + 1], S(-102));
  const uint8_t both_min_green[16] = {70, 0, CHANNEL_INDICES, S(-127), 0x80, CHANNEL_INDICES};
  DecodeBlock(Format::BC5_SNORM, both_min_green, out);
  CHECK_EQ(out[6 * 2 + 1], S(-127)); // index 6: -1.0 in the six-value mode
  CHECK_EQ(out[7 * 2 + 1], S(127));  // index 7: +1.0
}

/* ---- BC7 ------------------------------------------------------------------ */
//
// Interpolation: ((64 - w) * e0 + w * e1 + 32) >> 6.
// With e0 = 0 and e1 = 255 the weights give:
//   2-bit (0, 21, 43, 64):         0, 84, 171, 255
//   3-bit (0, 9, 18, ..., 64):     0, 36, 72, 108, 147, 183, 219, 255
//   4-bit (0, 4, 9, ..., 64):      0, 16, 36, 52, 68, 84, 104, 120,
//                                  135, 151, 171, 187, 203, 219, 239, 255
const uint8_t kW2[4] = {0, 84, 171, 255};
const uint8_t kW3[8] = {0, 36, 72, 108, 147, 183, 219, 255};
const uint8_t kW4[16] = {0, 16, 36, 52, 68, 84, 104, 120, 135, 151, 171, 187, 203, 219, 239, 255};

void
TestBC7Mode0() {
  // Mode 0: 3 subsets, 4-bit partition, RGB 4 bits + unique P-bit, 3-bit indices.
  // Partition 0: subsets {0,0,1,1, 0,0,1,1, 0,2,2,1, 2,2,2,2}; anchors 0, 3, 15.
  // Endpoints (4-bit value, P-bit) -> 5 bits -> 8 bits by replication:
  //   e0 of each subset: (0,0,0) P=1 -> 5-bit 1 -> 8      => (8, 8, 8)
  //   s0 e1: (15,0,0) P=0 -> 30 -> 247                    => (247, 0, 0)
  //   s1 e1: (0,15,0) P=0                                 => (0, 247, 0)
  //   s2 e1: (0,0,15) P=0                                 => (0, 0, 247)
  // Indices: anchors (texels 0, 3, 15) are 2 bits and 0; texel 1 is 3; all others 7.
  //   texel 1, w = 27: r = (37*8 + 27*247 + 32) >> 6 = 109, g = b = (37*8 + 32) >> 6 = 5.
  BlockWriter w;
  w.put(1, 1);
  w.put(0, 4);
  w.put_all({0, 15, 0, 0, 0, 0}, 4); // R
  w.put_all({0, 0, 0, 15, 0, 0}, 4); // G
  w.put_all({0, 0, 0, 0, 0, 15}, 4); // B
  w.put_all({1, 0, 1, 0, 1, 0}, 1);  // P
  for (int t = 0; t < 16; t++) {
    bool anchor = t == 0 || t == 3 || t == 15;
    w.put(anchor ? 0 : t == 1 ? 3 : 7, anchor ? 2 : 3);
  }
  CHECK_EQ(w.pos, 128);

  uint8_t out[64];
  DecodeBlock(Format::BC7, w.bytes, out);
  const uint8_t subset[16] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2};
  for (int t = 0; t < 16; t++) {
    if (t == 0 || t == 3 || t == 15)
      CHECK_RGBA(T(out, t), 8, 8, 8, 255);
    else if (t == 1)
      CHECK_RGBA(T(out, t), 109, 5, 5, 255);
    else if (subset[t] == 0)
      CHECK_RGBA(T(out, t), 247, 0, 0, 255);
    else if (subset[t] == 1)
      CHECK_RGBA(T(out, t), 0, 247, 0, 255);
    else
      CHECK_RGBA(T(out, t), 0, 0, 247, 255);
  }
}

void
TestBC7Mode1() {
  // Mode 1: 2 subsets, 6-bit partition, RGB 6 bits + shared P-bit per subset,
  // 3-bit indices. Partition 17: subsets {0,1,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0};
  // anchor of subset 1 is texel 2.
  // Endpoints (6-bit, P) -> 7 bits -> 8 bits:
  //   s0 (P=1): e0 (0,0,0) -> 1 -> 2 => (2,2,2);  e1 (63,63,63) -> 127 -> 255.
  //   s1 (P=0): e0 (63,0,0) -> 126 -> 253 => (253,0,0);  e1 (0,0,63) => (0,0,253).
  // Indices: texel 0 = 0 (2 bits), 1 = 0, 2 = 3 (2 bits), 3 = 7, 4 = 4, 7 = 5, others 7.
  //   texel 2 (s1, w=27): r = (37*253 + 32) >> 6 = 146, b = (27*253 + 32) >> 6 = 107.
  //   texel 4 (s0, w=37): (27*2 + 37*255 + 32) >> 6 = 148.
  //   texel 7 (s1, w=46): r = (18*253 + 32) >> 6 = 71, b = (46*253 + 32) >> 6 = 182.
  BlockWriter w;
  w.put(2, 2);
  w.put(17, 6);
  w.put_all({0, 63, 63, 0}, 6); // R
  w.put_all({0, 63, 0, 0}, 6);  // G
  w.put_all({0, 63, 0, 63}, 6); // B
  w.put_all({1, 0}, 1);         // shared P-bits
  const uint32_t index[16] = {0, 0, 3, 7, 4, 7, 7, 5, 7, 7, 7, 7, 7, 7, 7, 7};
  for (int t = 0; t < 16; t++)
    w.put(index[t], t == 0 || t == 2 ? 2 : 3);
  CHECK_EQ(w.pos, 128);

  uint8_t out[64];
  DecodeBlock(Format::BC7, w.bytes, out);
  CHECK_RGBA(T(out, 0), 2, 2, 2, 255);
  CHECK_RGBA(T(out, 1), 253, 0, 0, 255);
  CHECK_RGBA(T(out, 2), 146, 0, 107, 255);
  CHECK_RGBA(T(out, 3), 0, 0, 253, 255);
  CHECK_RGBA(T(out, 4), 148, 148, 148, 255);
  CHECK_RGBA(T(out, 5), 255, 255, 255, 255);
  CHECK_RGBA(T(out, 6), 255, 255, 255, 255);
  CHECK_RGBA(T(out, 7), 71, 0, 182, 255);
  for (int t = 8; t < 16; t++)
    CHECK_RGBA(T(out, t), 255, 255, 255, 255);
}

void
TestBC7Mode2() {
  // Mode 2: 3 subsets, 6-bit partition, RGB 5 bits, no P-bits, 2-bit indices.
  // Partition 13: subsets {0,1,2,2, 0,1,2,2, 0,1,2,2, 0,1,2,2}; anchors 0, 5, 15.
  // Endpoints (5 bits -> 8): 31 -> 255, 16 -> 132.
  //   s0: (0,0,0) .. (255,255,255)      -> grey ramp 0, 84, 171, 255
  //   s1: (255,0,0) .. (0,0,0)          -> r = 255, 171, 84, 0
  //   s2: (0,132,0) .. (0,0,255)        -> g = 132, 89, 43, 0; b = 0, 84, 171, 255
  //       (g: (43*132 + 32) >> 6 = 89, (21*132 + 32) >> 6 = 43)
  BlockWriter w;
  w.put(4, 3);
  w.put(13, 6);
  w.put_all({0, 31, 31, 0, 0, 0}, 5);  // R
  w.put_all({0, 31, 0, 0, 16, 0}, 5);  // G
  w.put_all({0, 31, 0, 0, 0, 31}, 5);  // B
  const uint32_t index[16] = {1, 2, 3, 0, 2, 1, 1, 3, 0, 3, 2, 0, 3, 0, 3, 1};
  for (int t = 0; t < 16; t++)
    w.put(index[t], t == 0 || t == 5 || t == 15 ? 1 : 2);
  CHECK_EQ(w.pos, 128);

  uint8_t out[64];
  DecodeBlock(Format::BC7, w.bytes, out);
  CHECK_RGBA(T(out, 0), 84, 84, 84, 255);
  CHECK_RGBA(T(out, 1), 84, 0, 0, 255);
  CHECK_RGBA(T(out, 2), 0, 0, 255, 255);
  CHECK_RGBA(T(out, 3), 0, 132, 0, 255);
  CHECK_RGBA(T(out, 4), 171, 171, 171, 255);
  CHECK_RGBA(T(out, 5), 171, 0, 0, 255);
  CHECK_RGBA(T(out, 6), 0, 89, 84, 255);
  CHECK_RGBA(T(out, 7), 0, 0, 255, 255);
  CHECK_RGBA(T(out, 8), 0, 0, 0, 255);
  CHECK_RGBA(T(out, 9), 0, 0, 0, 255);
  CHECK_RGBA(T(out, 10), 0, 43, 171, 255);
  CHECK_RGBA(T(out, 11), 0, 132, 0, 255);
  CHECK_RGBA(T(out, 12), 255, 255, 255, 255);
  CHECK_RGBA(T(out, 13), 255, 0, 0, 255);
  CHECK_RGBA(T(out, 14), 0, 0, 255, 255);
  CHECK_RGBA(T(out, 15), 0, 89, 84, 255);
}

void
TestBC7Mode3() {
  // Mode 3: 2 subsets, 6-bit partition, RGB 7 bits + unique P-bit, 2-bit indices.
  // Partition 18: subsets {0,0,0,0, 0,0,0,0, 1,0,0,0, 1,1,1,0}; anchor of subset 1 = 8.
  // Endpoints are exactly 8 bits (value << 1 | P):
  //   s0: (0,0,0) P=1 -> (1,1,1);  (127,127,127) P=1 -> (255,255,255)
  //   s1: (64,32,16) P=0 -> (128,64,32);  (64,32,16) P=1 -> (129,65,33)
  // s0 ramp: 1, (43 + 21*255 + 32) >> 6 = 84, (21 + 43*255 + 32) >> 6 = 172, 255.
  // s1 ramp: (128,64,32), (128,64,32), (129,65,33), (129,65,33).
  BlockWriter w;
  w.put(8, 4);
  w.put(18, 6);
  w.put_all({0, 127, 64, 64}, 7); // R
  w.put_all({0, 127, 32, 32}, 7); // G
  w.put_all({0, 127, 16, 16}, 7); // B
  w.put_all({1, 1, 0, 1}, 1);     // P
  const uint32_t index[16] = {1, 2, 3, 3, 3, 3, 3, 3, 0, 3, 3, 3, 3, 1, 2, 3};
  for (int t = 0; t < 16; t++)
    w.put(index[t], t == 0 || t == 8 ? 1 : 2);
  CHECK_EQ(w.pos, 128);

  uint8_t out[64];
  DecodeBlock(Format::BC7, w.bytes, out);
  CHECK_RGBA(T(out, 0), 84, 84, 84, 255);
  CHECK_RGBA(T(out, 1), 172, 172, 172, 255);
  for (int t : {2, 3, 4, 5, 6, 7, 9, 10, 11, 15})
    CHECK_RGBA(T(out, t), 255, 255, 255, 255);
  CHECK_RGBA(T(out, 8), 128, 64, 32, 255);
  CHECK_RGBA(T(out, 12), 129, 65, 33, 255);
  CHECK_RGBA(T(out, 13), 128, 64, 32, 255);
  CHECK_RGBA(T(out, 14), 129, 65, 33, 255);
}

void
TestBC7Mode4() {
  // Mode 4: 1 subset, rotation (2 bits), index selection (1 bit), RGB 5 bits,
  // alpha 6 bits, 2-bit indices (31 bits) then 3-bit indices (47 bits).
  // Endpoints: colour 0 .. 31 -> 0 .. 255, alpha 0 .. 63 -> 0 .. 255.
  // 2-bit index of texel t is t & 3, 3-bit index is t & 7.
  // Selection 0: colour uses the 2-bit indices, alpha the 3-bit ones; 1: swapped.
  for (uint32_t selection = 0; selection < 2; selection++) {
    BlockWriter w;
    w.put(16, 5);
    w.put(0, 2); // rotation
    w.put(selection, 1);
    w.put_all({0, 31, 0, 31, 0, 31}, 5); // R0 R1 G0 G1 B0 B1
    w.put_all({0, 63}, 6);               // A0 A1
    for (int t = 0; t < 16; t++)
      w.put(t & 3, t == 0 ? 1 : 2);
    for (int t = 0; t < 16; t++)
      w.put(t & 7, t == 0 ? 2 : 3);
    CHECK_EQ(w.pos, 128);

    uint8_t out[64];
    DecodeBlock(Format::BC7, w.bytes, out);
    for (int t = 0; t < 16; t++) {
      uint8_t c = selection == 0 ? kW2[t & 3] : kW3[t & 7];
      uint8_t a = selection == 0 ? kW3[t & 7] : kW2[t & 3];
      CHECK_RGBA(T(out, t), c, c, c, a);
    }
  }
}

void
TestBC7Mode5() {
  // Mode 5: 1 subset, rotation (2 bits), RGB 7 bits, alpha 8 bits, 2-bit colour
  // indices then 2-bit alpha indices. Colour 0 .. 127 -> 0 .. 255 (ramp kW2).
  // Alpha 64 .. 192: (4096 + 128w + 32) >> 6 = 64 + 2w -> 64, 106, 150, 192.
  // Colour index of texel t is t & 3, alpha index (t + 1) & 3 (texel 0: 1, 1 bit).
  // Rotation 1/2/3 swaps alpha with red/green/blue after interpolation.
  const uint8_t alpha[4] = {64, 106, 150, 192};
  for (uint32_t rotation = 0; rotation < 4; rotation++) {
    BlockWriter w;
    w.put(32, 6);
    w.put(rotation, 2);
    w.put_all({0, 127, 0, 127, 0, 127}, 7); // R0 R1 G0 G1 B0 B1
    w.put_all({64, 192}, 8);                // A0 A1
    for (int t = 0; t < 16; t++)
      w.put(t & 3, t == 0 ? 1 : 2);
    for (int t = 0; t < 16; t++)
      w.put((t + 1) & 3, t == 0 ? 1 : 2);
    CHECK_EQ(w.pos, 128);

    uint8_t out[64];
    DecodeBlock(Format::BC7, w.bytes, out);
    for (int t = 0; t < 16; t++) {
      uint8_t px[4] = {kW2[t & 3], kW2[t & 3], kW2[t & 3], alpha[(t + 1) & 3]};
      if (rotation) {
        uint8_t tmp = px[rotation - 1];
        px[rotation - 1] = px[3];
        px[3] = tmp;
      }
      CHECK_RGBA(T(out, t), px[0], px[1], px[2], px[3]);
    }
  }

  // The same, spelled out for rotation 1, texels 0..3:
  //   before: (0,0,0,106) (84,84,84,150) (171,171,171,192) (255,255,255,64)
  BlockWriter w;
  w.put(32, 6);
  w.put(1, 2);
  w.put_all({0, 127, 0, 127, 0, 127}, 7);
  w.put_all({64, 192}, 8);
  for (int t = 0; t < 16; t++)
    w.put(t & 3, t == 0 ? 1 : 2);
  for (int t = 0; t < 16; t++)
    w.put((t + 1) & 3, t == 0 ? 1 : 2);
  uint8_t out[64];
  DecodeBlock(Format::BC7, w.bytes, out);
  CHECK_RGBA(T(out, 0), 106, 0, 0, 0);
  CHECK_RGBA(T(out, 1), 150, 84, 84, 84);
  CHECK_RGBA(T(out, 2), 192, 171, 171, 171);
  CHECK_RGBA(T(out, 3), 64, 255, 255, 255);
}

void
TestBC7Mode6() {
  // Mode 6: 1 subset, RGBA 7 bits + unique P-bit, 4-bit indices (texel 0: 3 bits).
  //   e0 = (0,0,0,127) P=0 -> (0,0,0,254);  e1 = (127,127,127,0) P=1 -> (255,255,255,1)
  // Texel t has index t: colour = kW4[t];
  //   alpha t=0: 254; t=7 (w=30): (34*254 + 30 + 32) >> 6 = 135;
  //   t=8 (w=34): (30*254 + 34 + 32) >> 6 = 120; t=15: (64 + 32) >> 6 = 1.
  BlockWriter w;
  w.put(64, 7);
  w.put_all({0, 127, 0, 127, 0, 127, 127, 0}, 7); // R0 R1 G0 G1 B0 B1 A0 A1
  w.put_all({0, 1}, 1);
  for (int t = 0; t < 16; t++)
    w.put(t, t == 0 ? 3 : 4);
  CHECK_EQ(w.pos, 128);

  uint8_t out[64];
  DecodeBlock(Format::BC7, w.bytes, out);
  for (int t = 0; t < 16; t++) {
    CHECK_EQ(out[t * 4 + 0], kW4[t]);
    CHECK_EQ(out[t * 4 + 1], kW4[t]);
    CHECK_EQ(out[t * 4 + 2], kW4[t]);
  }
  CHECK_EQ(out[0 * 4 + 3], 254);
  CHECK_EQ(out[7 * 4 + 3], 135);
  CHECK_EQ(out[8 * 4 + 3], 120);
  CHECK_EQ(out[15 * 4 + 3], 1);
}

void
TestBC7Mode7() {
  // Mode 7: 2 subsets, 6-bit partition, RGBA 5 bits + unique P-bit, 2-bit indices.
  // Partition 1: subsets {0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1}; anchor of subset 1 = 15.
  // Endpoints (5-bit, P) -> 6 bits -> 8 bits ((v << 2) | (v >> 4)):
  //   s0 e0 (0,0,0,31) P=0 -> (0,0,0,62)  -> (0,0,0,251)
  //   s0 e1 (31,0,0,31) P=1 -> (63,1,1,63) -> (255,4,4,255)
  //   s1 e0 (0,0,0,0) P=0 -> (0,0,0,0)
  //   s1 e1 (0,0,31,16) P=1 -> (1,1,63,33) -> (4,4,255,134)
  // s0 index 1 (w=21): (84, 1, 1, (43*251 + 21*255 + 32) >> 6 = 252)
  // s0 index 2 (w=43): (171, 3, 3, (21*251 + 43*255 + 32) >> 6 = 254)
  // s1 index 1: (1, 1, 84, (21*134 + 32) >> 6 = 44); index 2: (3, 3, 171, 90)
  BlockWriter w;
  w.put(128, 8);
  w.put(1, 6);
  w.put_all({0, 31, 0, 0}, 5);   // R
  w.put_all({0, 0, 0, 0}, 5);    // G
  w.put_all({0, 0, 0, 31}, 5);   // B
  w.put_all({31, 31, 0, 16}, 5); // A
  w.put_all({0, 1, 0, 1}, 1);    // P
  const uint32_t index[16] = {0, 3, 1, 0, 2, 3, 3, 3, 3, 3, 3, 2, 3, 3, 3, 1};
  for (int t = 0; t < 16; t++)
    w.put(index[t], t == 0 || t == 15 ? 1 : 2);
  CHECK_EQ(w.pos, 128);

  uint8_t out[64];
  DecodeBlock(Format::BC7, w.bytes, out);
  CHECK_RGBA(T(out, 0), 0, 0, 0, 251);
  CHECK_RGBA(T(out, 1), 255, 4, 4, 255);
  CHECK_RGBA(T(out, 2), 84, 1, 1, 252);
  CHECK_RGBA(T(out, 3), 0, 0, 0, 0);
  CHECK_RGBA(T(out, 4), 171, 3, 3, 254);
  for (int t : {5, 6, 8, 9, 10, 12, 13, 14})
    CHECK_RGBA(T(out, t), 255, 4, 4, 255);
  CHECK_RGBA(T(out, 7), 4, 4, 255, 134);
  CHECK_RGBA(T(out, 11), 3, 3, 171, 90);
  CHECK_RGBA(T(out, 15), 1, 1, 84, 44);
}

void
TestBC7Reserved() {
  // No mode bit in the first byte: reserved mode, decodes to transparent black.
  uint8_t block[16];
  std::memset(block, 0, sizeof(block));
  block[5] = 0xFF;
  uint8_t out[64];
  std::memset(out, 0xCD, sizeof(out));
  DecodeBlock(Format::BC7, block, out);
  for (int t = 0; t < 16; t++)
    CHECK_RGBA(T(out, t), 0, 0, 0, 0);
}

/* ---- Whole subresources ----------------------------------------------------- */

void
TestSubresourceNotMultipleOf4() {
  // 5 x 3 BC1: two blocks across, one block down; source rows padded to 20 bytes.
  // Block 0: solid red (0xF800 == 0xF800, index 0); block 1: solid blue.
  uint8_t src[20] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0, //
                     0x1F, 0x00, 0x1F, 0x00, 0, 0, 0, 0, //
                     0xEE, 0xEE, 0xEE, 0xEE};
  // Tightly packed destination: 5 * 4 = 20 bytes per row, 3 rows, plus a guard.
  std::vector<uint8_t> dst(3 * 20 + 16, 0xCD);
  CHECK(DecodeSubresource(Format::BC1, src, sizeof(src), dst.data(), 0, 5, 3));
  for (int y = 0; y < 3; y++) {
    for (int x = 0; x < 4; x++)
      CHECK_RGBA(&dst[y * 20 + x * 4], 255, 0, 0, 255);
    CHECK_RGBA(&dst[y * 20 + 16], 0, 0, 255, 255);
  }
  for (size_t i = 3 * 20; i < dst.size(); i++)
    CHECK_EQ(dst[i], 0xCD); // texels outside the level aren't written

  // Explicit destination pitch of 32: bytes 20..31 of each row stay untouched.
  std::vector<uint8_t> padded(3 * 32, 0xCD);
  CHECK(DecodeSubresource(Format::BC1, src, sizeof(src), padded.data(), 32, 5, 3));
  for (int y = 0; y < 3; y++) {
    CHECK_RGBA(&padded[y * 32 + 0], 255, 0, 0, 255);
    CHECK_RGBA(&padded[y * 32 + 16], 0, 0, 255, 255);
    for (int i = 20; i < 32; i++)
      CHECK_EQ(padded[y * 32 + i], 0xCD);
  }
}

void
TestSubresourceRowsOfBlocks() {
  // 4 x 8 BC4: two rows of blocks with a source pitch of 12 (8 + 4 padding).
  // Row 0 of blocks: e0 = e1 = 10, all index 0 -> 10.
  // Row 1 of blocks: six-value block with e0 = e1 = 99 and every index 7 -> 255.
  const uint8_t src[24] = {10, 10, 0, 0, 0, 0, 0, 0, 0xAA, 0xAA, 0xAA, 0xAA,
                           99, 99, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xAA, 0xAA, 0xAA, 0xAA};
  uint8_t dst[4 * 8];
  CHECK(DecodeSubresource(Format::BC4_UNORM, src, 12, dst, 0, 4, 8));
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 4; x++)
      CHECK_EQ(dst[y * 4 + x], y < 4 ? 10 : 255);

  // 2 x 2 level (a small mip): one block, only its top-left 2 x 2 texels kept.
  const uint8_t small[8] = {210, 0, CHANNEL_INDICES};
  uint8_t tiny[4 + 4];
  std::memset(tiny, 0xCD, sizeof(tiny));
  CHECK(DecodeSubresource(Format::BC4_UNORM, small, 8, tiny, 0, 2, 2));
  CHECK_EQ(tiny[0], kEightValueUnorm[0]); // texel (0,0), index 0
  CHECK_EQ(tiny[1], kEightValueUnorm[1]); // texel (1,0), index 1
  CHECK_EQ(tiny[2], kEightValueUnorm[4]); // texel (0,1), index 4
  CHECK_EQ(tiny[3], kEightValueUnorm[5]); // texel (1,1), index 5
  for (int i = 4; i < 8; i++)
    CHECK_EQ(tiny[i], 0xCD);
}

void
TestSubresourceErrors() {
  uint8_t src[16] = {};
  uint8_t dst[64];
  std::memset(dst, 0xCD, sizeof(dst));
  // Source pitch too small for two BC1 blocks (16 bytes).
  CHECK(!DecodeSubresource(Format::BC1, src, 15, dst, 0, 8, 4));
  CHECK(!DecodeSubresource(Format::BC1, nullptr, 16, dst, 0, 8, 4));
  CHECK(!DecodeSubresource(Format::BC1, src, 16, nullptr, 0, 8, 4));
  for (uint8_t b : dst)
    CHECK_EQ(b, 0xCD);
  // An empty level succeeds and writes nothing.
  CHECK(DecodeSubresource(Format::BC1, src, 0, dst, 0, 0, 4));
  CHECK_EQ(dst[0], 0xCD);
}

void
TestVolume() {
  // 4 x 4 x 2 BC4 (spec 05 clarification 3): each depth slice is its own 2D
  // BC image. One row of blocks per slice, slices 12 bytes apart (8 + 4 padding).
  // Slice 0: e0 = e1 = 10, all index 0 -> 10. Slice 1: e0 = e1 = 99, all index 7 -> 255.
  const uint8_t src[24] = {10, 10, 0, 0, 0, 0, 0, 0, 0xAA, 0xAA, 0xAA, 0xAA,
                           99, 99, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xAA, 0xAA, 0xAA, 0xAA};
  uint8_t dst[2 * 16 + 8];
  std::memset(dst, 0xCD, sizeof(dst));
  CHECK(dxmt::bc::DecodeVolume(Format::BC4_UNORM, src, 8, 12, dst, 0, 0, 4, 4, 2));
  for (int i = 0; i < 16; i++) {
    CHECK_EQ(dst[i], 10);       // slice 0, tightly packed
    CHECK_EQ(dst[16 + i], 255); // slice 1 right after it
  }
  for (int i = 32; i < 40; i++)
    CHECK_EQ(dst[i], 0xCD);

  // 5 x 3 x 2 BC1 with padded destination rows (32) and slices (100 bytes):
  // slice 0 solid red, slice 1 solid blue; texels outside the level untouched.
  const uint8_t red[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
  const uint8_t blue[8] = {0x1F, 0x00, 0x1F, 0x00, 0, 0, 0, 0};
  uint8_t bc1[2 * 16];
  std::memcpy(bc1 + 0, red, 8);
  std::memcpy(bc1 + 8, red, 8);
  std::memcpy(bc1 + 16, blue, 8);
  std::memcpy(bc1 + 24, blue, 8);
  std::vector<uint8_t> out(200, 0xCD);
  CHECK(dxmt::bc::DecodeVolume(Format::BC1, bc1, 16, 16, out.data(), 32, 100, 5, 3, 2));
  for (int y = 0; y < 3; y++) {
    for (int x = 0; x < 5; x++) {
      CHECK_RGBA(&out[y * 32 + x * 4], 255, 0, 0, 255);
      CHECK_RGBA(&out[100 + y * 32 + x * 4], 0, 0, 255, 255);
    }
    CHECK_EQ(out[y * 32 + 20], 0xCD);
  }
  CHECK_EQ(out[96], 0xCD); // between the slices

  // A depth pitch smaller than one slice of blocks: refused, nothing written.
  uint8_t untouched[64];
  std::memset(untouched, 0xCD, sizeof(untouched));
  CHECK(!dxmt::bc::DecodeVolume(Format::BC4_UNORM, src, 8, 4, untouched, 0, 0, 4, 4, 2));
  for (uint8_t b : untouched)
    CHECK_EQ(b, 0xCD);
  // One slice needs no depth pitch.
  CHECK(dxmt::bc::DecodeVolume(Format::BC4_UNORM, src, 8, 0, untouched, 0, 0, 4, 4, 1));
  CHECK_EQ(untouched[0], 10);
  CHECK_EQ(untouched[16], 0xCD);
}

void
TestSizes() {
  CHECK_EQ(dxmt::bc::BlockBytes(Format::BC1), 8);
  CHECK_EQ(dxmt::bc::BlockBytes(Format::BC4_UNORM), 8);
  CHECK_EQ(dxmt::bc::BlockBytes(Format::BC4_SNORM), 8);
  CHECK_EQ(dxmt::bc::BlockBytes(Format::BC2), 16);
  CHECK_EQ(dxmt::bc::BlockBytes(Format::BC3), 16);
  CHECK_EQ(dxmt::bc::BlockBytes(Format::BC5_SNORM), 16);
  CHECK_EQ(dxmt::bc::BlockBytes(Format::BC7), 16);
  CHECK_EQ(dxmt::bc::DecodedBytesPerTexel(Format::BC1), 4);
  CHECK_EQ(dxmt::bc::DecodedBytesPerTexel(Format::BC4_SNORM), 1);
  CHECK_EQ(dxmt::bc::DecodedBytesPerTexel(Format::BC5_UNORM), 2);
  CHECK_EQ(dxmt::bc::DecodedBytesPerTexel(Format::BC7), 4);
}

void
TestBC6HGrey() {
  // RGBA16Float (0.25, 0.25, 0.25, 1.0): halves 0x3400 and 0x3C00, little-endian.
  const uint8_t grey[8] = {0x00, 0x34, 0x00, 0x34, 0x00, 0x34, 0x00, 0x3C};
  uint8_t tight[3 * 2 * 8];
  dxmt::bc::FillBC6HGrey(tight, 0, 3, 2);
  for (int t = 0; t < 6; t++)
    CHECK(std::memcmp(tight + t * 8, grey, 8) == 0);

  uint8_t padded[2 * 32];
  std::memset(padded, 0xCD, sizeof(padded));
  dxmt::bc::FillBC6HGrey(padded, 32, 3, 2);
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 3; x++)
      CHECK(std::memcmp(padded + y * 32 + x * 8, grey, 8) == 0);
    for (int i = 24; i < 32; i++)
      CHECK_EQ(padded[y * 32 + i], 0xCD);
  }
}

} // namespace

int
main() {
  TestBC1FourColour();
  TestBC1Expansion565AndRounding();
  TestBC1ThreeColour();
  TestBC2();
  TestBC3();
  TestBC4Unorm();
  TestBC4Snorm();
  TestBC5();
  TestBC7Mode0();
  TestBC7Mode1();
  TestBC7Mode2();
  TestBC7Mode3();
  TestBC7Mode4();
  TestBC7Mode5();
  TestBC7Mode6();
  TestBC7Mode7();
  TestBC7Reserved();
  TestSubresourceNotMultipleOf4();
  TestSubresourceRowsOfBlocks();
  TestSubresourceErrors();
  TestVolume();
  TestSizes();
  TestBC6HGrey();
  return TestSummary("test_bc_decode");
}
