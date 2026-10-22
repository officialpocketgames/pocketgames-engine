// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 Part C, Windows half. On a GPU without BC support the unix half
// creates every BC texture with an uncompressed "physical" format (table in
// BCPhysicalFormat). Everything the game sees stays BC; only the Metal texture
// and the uploads into it use the physical format. These helpers let the
// upload paths decode block data before it reaches such a texture.

#pragma once

#include "Metal.hpp"
#include <cstddef>
#include <cstdint>

namespace dxmt {

/* True when BC textures are emulated: DXMT_IOS and the device answers
   supportsBCTextureCompression == NO. Worked out once and cached. */
bool BCEmulationActive(WMT::Device device);

/* True for a BC format whose texture is emulated (BCEmulationActive). */
bool IsEmulatedBCFormat(WMT::Device device, WMTPixelFormat format);

/* The uncompressed format the unix half uses for a BC format:
   BC1/2/3/7 -> RGBA8Unorm (sRGB -> RGBA8Unorm_sRGB), BC4 -> R8Unorm/R8Snorm,
   BC5 -> RG8Unorm/RG8Snorm, BC6H -> RGBA16Float. Other formats unchanged.
   Must agree with dxmt_ios_physical_format in the unix half. */
WMTPixelFormat BCPhysicalFormat(WMTPixelFormat format);

/* Bytes per texel of BCPhysicalFormat(format) (0 if not a BC format). */
uint32_t BCPhysicalBytesPerTexel(WMTPixelFormat format);

/* Bytes of one block of a BC format (8 or 16; 0 if not a BC format). */
uint32_t BCBlockBytes(WMTPixelFormat format);

/* Decode a width x height area of block data (src_row_pitch = bytes per row of
   blocks) into the physical format at dst (dst_row_pitch 0 = tightly packed).
   BC6H is not decoded: the area is filled with opaque grey. Allocates nothing.
   Returns false if the format isn't BC or the source pitch is too small. */
bool BCDecodeToPhysical(
    WMTPixelFormat format, const void *src, size_t src_row_pitch, void *dst, size_t dst_row_pitch, uint32_t width,
    uint32_t height
);

/* The same for `depth` slices (3D textures, spec 05 clarification 3): source
   slices src_depth_pitch apart, decoded slices tightly packed at dst (rows of
   width * BCPhysicalBytesPerTexel bytes). BC6H fills every slice with grey. */
bool BCDecodeVolumeToPhysical(
    WMTPixelFormat format, const void *src, size_t src_row_pitch, size_t src_depth_pitch, void *dst, uint32_t width,
    uint32_t height, uint32_t depth
);

} // namespace dxmt
