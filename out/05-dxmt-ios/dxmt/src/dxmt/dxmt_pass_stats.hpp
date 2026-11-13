// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 63: what the encoding thread counts for the Direct3D 11 call
// statistics (DXMT_D11_CALL_STATS): the encoders left after the optimiser, the clears folded
// and the passes merged, items 69-72's cuts (applied, and would apply with the switch off) with
// the bytes of loads and stores they avoid, and the encoding thread's time. Off (one relaxed
// load) unless a Direct3D 11 device turned it on; the report takes and resets it.

#pragma once

#include <atomic>
#include <cstdint>

namespace dxmt {

/* items 69-72, in clarification 63's order */
enum class PassCut : uint8_t { KeepDsv, RoStore, Discard, OverwriteLoad, Count };

struct EncoderPassStats {
  std::atomic<uint64_t> render{0}, compute{0}, blit{0}, clear{0};
  std::atomic<uint64_t> folded_clears{0}, merged_render{0}, merged_compute{0}, merged_blit{0};
  std::atomic<uint64_t> cut_applied[unsigned(PassCut::Count)] = {};
  std::atomic<uint64_t> cut_would[unsigned(PassCut::Count)] = {};
  std::atomic<uint64_t> bytes_avoided{0}, bytes_avoidable{0};
  std::atomic<uint64_t> encode_ns{0};
};

inline EncoderPassStats g_encoder_pass_stats;
inline std::atomic<bool> g_encoder_pass_stats_on{false};

inline bool
EncoderPassStatsOn() {
  return g_encoder_pass_stats_on.load(std::memory_order_relaxed);
}

inline void
PassStatAdd(std::atomic<uint64_t> &counter, uint64_t n = 1) {
  if (EncoderPassStatsOn())
    counter.fetch_add(n, std::memory_order_relaxed);
}

/* A counter's value since the last take, reset to 0. */
inline uint64_t
PassStatTake(std::atomic<uint64_t> &counter) {
  return counter.exchange(0, std::memory_order_relaxed);
}

} // namespace dxmt
