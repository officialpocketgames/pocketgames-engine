// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 D5: mip clamp for BC textures. A clamped texture leaves its top
// `bias` mip levels out of the Metal texture; physical level p is logical
// level p + bias. This header holds the policy as pure functions (no Windows
// or Metal types) so it can be unit-tested on any host.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt::mipclamp {

/* The largest bias <= requested that leaves at least one level and a physical
   top level of at least 4x4 whose width and height are multiples of 4. */
inline uint32_t
ChooseBias(uint32_t width, uint32_t height, uint32_t levels, uint32_t requested) {
  if (levels == 0)
    return 0;
  uint32_t bias = requested < levels - 1 ? requested : levels - 1;
  for (; bias > 0; bias--) {
    uint32_t w = width >> bias, h = height >> bias;
    if (w >= 4 && h >= 4 && w % 4 == 0 && h % 4 == 0)
      return bias;
  }
  return 0;
}

/* The automatic clamp only looks at textures with width or height >= 1024. */
inline bool
IsAutoCandidate(uint32_t width, uint32_t height) {
  return width >= 1024 || height >= 1024;
}

/* State of the automatic clamp, shared by all textures of the process. */
struct AutoState {
  uint64_t candidates = 0;     /* candidates seen so far */
  bool have_reading = false;   /* a headroom reading exists */
  uint64_t last_headroom = 0;  /* bytes */
  bool query_unavailable = false;
};

/* Requery rule: ask for headroom on the first candidate, every 16th, and on
   every one while the last reading was below twice the threshold.
   `index` is the 0-based number of the candidate. */
inline bool
ShouldRequery(uint64_t index, bool have_reading, uint64_t last_headroom, uint64_t threshold) {
  if (index == 0 || !have_reading)
    return true;
  if (index % 16 == 0)
    return true;
  return last_headroom < 2 * threshold;
}

/* Pressure rule: bias 1 while the headroom is below the threshold. */
inline uint32_t
AutoBias(uint64_t headroom, uint64_t threshold) {
  return headroom < threshold ? 1 : 0;
}

/* The bias asked for, before ChooseBias:
   - explicit (d3d11.mipClampBC) > 0 wins;
   - otherwise the automatic clamp, if enabled, for candidates, using the
     latest headroom reading (query(headroom_out) returns false when the
     query isn't available; then the automatic clamp never clamps). */
template <typename QueryHeadroom>
inline uint32_t
RequestedBias(
    uint32_t explicit_bias, bool auto_enabled, uint64_t threshold, uint32_t width, uint32_t height, AutoState &state,
    QueryHeadroom &&query
) {
  if (explicit_bias > 0)
    return explicit_bias;
  if (!auto_enabled || state.query_unavailable || !IsAutoCandidate(width, height))
    return 0;
  uint64_t index = state.candidates++;
  if (ShouldRequery(index, state.have_reading, state.last_headroom, threshold)) {
    uint64_t headroom = 0;
    if (!query(headroom)) {
      state.query_unavailable = true;
      return 0;
    }
    state.have_reading = true;
    state.last_headroom = headroom;
  }
  return AutoBias(state.last_headroom, threshold);
}

/* A view's mip range translated to physical levels, clamped into range.
   Logical [first, first + count) -> physical [first - bias, ...) intersected
   with [0, physical_levels); never empty. */
inline void
TranslateViewRange(
    uint32_t bias, uint32_t physical_levels, uint32_t first, uint32_t count, uint32_t &out_first, uint32_t &out_count
) {
  int64_t lo = int64_t(first) - bias;
  int64_t hi = int64_t(first) + count - bias; /* exclusive */
  if (lo < 0)
    lo = 0;
  if (hi > int64_t(physical_levels))
    hi = physical_levels;
  if (lo >= int64_t(physical_levels))
    lo = physical_levels ? physical_levels - 1 : 0;
  if (hi <= lo)
    hi = lo + 1;
  out_first = uint32_t(lo);
  out_count = uint32_t(hi - lo);
}

/* The minimum LOD for a view starting at physical level `first` with `count`
   levels: never start at a lost level; raise it to the first level at or
   below `first` (towards smaller mips) that isn't lost. `lost` has bit p set
   for lost physical level p. */
inline float
MinLodForLostLevels(uint32_t lost, uint32_t first, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    uint32_t level = first + i;
    if (level >= 32 || !(lost & (1u << level)))
      return float(i);
  }
  return count ? float(count - 1) : 0.0f;
}

/* 2x2 box filter of 8-bit texels with `channels` components (1, 2 or 4):
   destination size max(1, w/2) x max(1, h/2); a source row or column beyond
   the edge (odd or 1-texel sizes) repeats the last one. Unsigned: rounded
   mean. Signed (two's complement snorm): mean truncated toward zero.
   Pitches are in bytes; 0 means tightly packed. */
inline void
BoxFilter2x2(
    const uint8_t *src, size_t src_pitch, uint32_t width, uint32_t height, uint8_t *dst, size_t dst_pitch,
    uint32_t channels, bool is_signed
) {
  uint32_t dw = width > 1 ? width / 2 : 1, dh = height > 1 ? height / 2 : 1;
  if (!src_pitch)
    src_pitch = size_t(width) * channels;
  if (!dst_pitch)
    dst_pitch = size_t(dw) * channels;
  for (uint32_t y = 0; y < dh; y++) {
    uint32_t y0 = 2 * y < height ? 2 * y : height - 1;
    uint32_t y1 = 2 * y + 1 < height ? 2 * y + 1 : height - 1;
    for (uint32_t x = 0; x < dw; x++) {
      uint32_t x0 = 2 * x < width ? 2 * x : width - 1;
      uint32_t x1 = 2 * x + 1 < width ? 2 * x + 1 : width - 1;
      for (uint32_t c = 0; c < channels; c++) {
        const uint8_t *r0 = src + size_t(y0) * src_pitch, *r1 = src + size_t(y1) * src_pitch;
        uint8_t a = r0[x0 * channels + c], b = r0[x1 * channels + c];
        uint8_t d = r1[x0 * channels + c], e = r1[x1 * channels + c];
        uint8_t out;
        if (is_signed) {
          int sum = int(int8_t(a)) + int(int8_t(b)) + int(int8_t(d)) + int(int8_t(e));
          out = uint8_t(int8_t(sum / 4)); /* C++ division truncates toward zero */
        } else {
          out = uint8_t((uint32_t(a) + b + d + e + 2) / 4);
        }
        dst[size_t(y) * dst_pitch + size_t(x) * channels + c] = out;
      }
    }
  }
}

} // namespace dxmt::mipclamp
