// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 90 (round 20): ledger records (dxmt_mem_ledger.hpp) filled from DXMT's own types,
// for the places that make Metal textures and buffers.
#pragma once

#include "Metal.hpp"
#include "dxmt_mem_ledger.hpp"
#include <algorithm>
#include <cstdint>

namespace dxmt::memledger {

/* Metal's allocatedSize isn't read back for buffers (WMTBufferInfo has no room for it): a
   buffer counts as its length in whole 16 KB pages, the page size of these GPUs' memory. */
constexpr uint64_t kBufferPage = 16384;

inline uint64_t
BufferBytes(uint64_t length) {
  return (length + kBufferPage - 1) / kBufferPage * kBufferPage;
}

/* Private for private and memoryless storage; shared for shared and managed (managed is shared
   on iOS, spec 05 B1) */
inline Storage
StorageOf(uint64_t options) {
  uint64_t mode = options & (WMTResourceStorageModeManaged | WMTResourceStorageModePrivate);
  return mode == WMTResourceStorageModePrivate || mode == WMTResourceStorageModeMemoryless ? Storage::Private
                                                                                            : Storage::Shared;
}

inline Info
BufferRecord(Kind kind, uint64_t length, uint64_t options, const char *what = nullptr) {
  Info info;
  info.kind = kind;
  info.storage = StorageOf(options);
  info.texture = false;
  info.width = uint32_t(std::min<uint64_t>(length, UINT32_MAX));
  info.bytes = BufferBytes(length);
  info.what = what;
  return info;
}

/* The size the unix side wrote back (WMTTextureInfo::allocated_kb: Metal's allocatedSize) */
inline uint64_t
TextureBytes(const WMTTextureInfo &info) {
  return uint64_t(info.allocated_kb) << 10;
}

/* A texture as the program asked for it (`info`: the program's format, not the physical one);
   render target or depth-stencil when Metal may render to it. */
inline Info
TextureRecord(const WMTTextureInfo &info, uint64_t bytes, bool bc_uncompressed, const char *what = nullptr) {
  Info record;
  record.kind = (info.usage & WMTTextureUsageRenderTarget) ? Kind::RtDs : Kind::Texture;
  record.storage = StorageOf(info.options);
  record.texture = true;
  record.bc_uncompressed = bc_uncompressed;
  record.mips = uint8_t(info.mipmap_level_count);
  record.samples = uint8_t(info.sample_count);
  record.texture_type = uint8_t(info.type);
  record.format = uint32_t(info.pixel_format);
  record.width = info.width;
  record.height = info.height;
  record.depth = info.depth;
  record.array_length = info.array_length;
  record.bytes = bytes;
  record.what = what;
  return record;
}

} // namespace dxmt::memledger
