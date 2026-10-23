// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 Part C: CPU decoder for the block-compressed formats BC1, BC2, BC3,
// BC4 (unsigned/signed), BC5 (unsigned/signed) and BC7, following Microsoft's
// "Texture Block Compression in Direct3D 11" documentation. Plain C++20 with no
// Windows or Metal types, so it can be unit-tested on any host. Nothing here
// allocates memory.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt::bc {

enum class Format : uint8_t {
  BC1,       // -> RGBA8 (4 bytes per texel)
  BC2,       // -> RGBA8
  BC3,       // -> RGBA8
  BC4_UNORM, // -> R8 unorm (1 byte)
  BC4_SNORM, // -> R8 snorm (1 byte, two's complement, -127..127)
  BC5_UNORM, // -> RG8 unorm (2 bytes)
  BC5_SNORM, // -> RG8 snorm (2 bytes)
  BC7,       // -> RGBA8
};

/* Bytes of one 4x4 block on the wire. */
constexpr uint32_t
BlockBytes(Format format) {
  switch (format) {
  case Format::BC1:
  case Format::BC4_UNORM:
  case Format::BC4_SNORM:
    return 8;
  default:
    return 16;
  }
}

/* Bytes of one decoded texel. */
constexpr uint32_t
DecodedBytesPerTexel(Format format) {
  switch (format) {
  case Format::BC4_UNORM:
  case Format::BC4_SNORM:
    return 1;
  case Format::BC5_UNORM:
  case Format::BC5_SNORM:
    return 2;
  default:
    return 4;
  }
}

/* Decode one block into 16 texels, row-major (texel (x, y) at index y * 4 + x),
   DecodedBytesPerTexel(format) bytes each. */
void DecodeBlock(Format format, const uint8_t *block, uint8_t *texels);

/* Decode a whole subresource.
   src, src_row_pitch: the block data; the pitch is bytes per row of blocks
                       (what D3D's row pitch is for BC formats).
   dst, dst_row_pitch: decoded texels; a pitch of 0 means tightly packed
                       (width * DecodedBytesPerTexel(format)).
   width, height:      the level's size in texels. A size that isn't a multiple
                       of 4 is still whole blocks in src; only the texels inside
                       width x height are written.
   Returns false (and writes nothing) for a null pointer or a source pitch too
   small for the row of blocks. */
bool DecodeSubresource(
    Format format, const void *src, size_t src_row_pitch, void *dst, size_t dst_row_pitch, uint32_t width,
    uint32_t height
);

/* Decode a 3D subresource (spec 05 clarification 3): each depth slice is an
   independent 2D BC image, its rows of blocks src_row_pitch apart, the slices
   src_depth_pitch apart. Decoded slices are dst_depth_pitch apart (0 = tightly
   packed: dst_row_pitch, or width * DecodedBytesPerTexel when that is 0, times
   height). Returns false (and writes nothing) under the same conditions as
   DecodeSubresource, or when depth > 1 and src_depth_pitch is smaller than one
   slice of blocks. */
bool DecodeVolume(
    Format format, const void *src, size_t src_row_pitch, size_t src_depth_pitch, void *dst, size_t dst_row_pitch,
    size_t dst_depth_pitch, uint32_t width, uint32_t height, uint32_t depth
);

/* BC6H isn't decoded: fill an RGBA16Float area with opaque grey
   (0.25, 0.25, 0.25, 1.0). A pitch of 0 means tightly packed (width * 8). */
void FillBC6HGrey(void *dst, size_t dst_row_pitch, uint32_t width, uint32_t height);

} // namespace dxmt::bc
