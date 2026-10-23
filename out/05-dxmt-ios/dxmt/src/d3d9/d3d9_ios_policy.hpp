// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 Part C: the decision rules of the Direct3D 9 frontend's iOS
// behaviour, kept free of Windows, Metal and device types so the host unit
// tests (tests/test_d3d9_policy.cpp) can build them as they are. The device
// code in d3d9_device.cpp, d3d9_query.cpp, d3d9_texture.cpp,
// d3d9_cube_texture.cpp and d3d9_interface.cpp calls these; the switches
// themselves are read once in d3d9_ios.cpp.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace dxmt::d9ios {

/* ---- switches -------------------------------------------------------------
   Spec 07 C: unset or empty means the default, "0" means off, anything else
   means on. */
inline bool
ParseSwitch(const char *value, bool default_on) {
  if (value == nullptr || value[0] == '\0')
    return default_on;
  return !(value[0] == '0' && value[1] == '\0');
}

/* A decimal count. Unset, empty or not a plain decimal number: `fallback`. */
inline uint64_t
ParseCount(const char *value, uint64_t fallback) {
  if (value == nullptr || value[0] == '\0')
    return fallback;
  uint64_t n = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9')
      return fallback;
    if (n > (UINT64_MAX - 9) / 10)
      return fallback;
    n = n * 10 + uint64_t(*p - '0');
  }
  return n;
}

/* ---- C3: upload pressure -------------------------------------------------- */

constexpr uint64_t kDefaultUploadCommitMB = 64;

/* DXMT_D9_UPLOAD_COMMIT_MB in bytes; 0 means never commit for pressure. */
inline uint64_t
UploadCommitThresholdBytes(const char *value) {
  uint64_t mb = ParseCount(value, kDefaultUploadCommitMB);
  if (mb > (UINT64_MAX >> 20))
    mb = UINT64_MAX >> 20;
  return mb << 20;
}

/* Bytes staged for uploads since the last commit, and the sequence number the
   last pressure commit signals (0: none yet). The device owns one, under its
   lock. */
class UploadPressure {
public:
  explicit UploadPressure(uint64_t threshold_bytes = 0) : threshold_(threshold_bytes) {}

  void
  setThreshold(uint64_t threshold_bytes) {
    threshold_ = threshold_bytes;
  }

  uint64_t
  threshold() const {
    return threshold_;
  }

  void
  noteStaged(uint64_t bytes) {
    staged_ = bytes > UINT64_MAX - staged_ ? UINT64_MAX : staged_ + bytes;
  }

  uint64_t
  staged() const {
    return staged_;
  }

  /* At a safe boundary: is a pressure commit due? */
  bool
  due() const {
    return threshold_ != 0 && staged_ >= threshold_;
  }

  /* Every commit, whatever caused it, starts a new count. */
  void
  onCommit() {
    staged_ = 0;
  }

  /* The sequence number to wait for before the next pressure commit (0: no
     wait). */
  uint64_t
  previousCommitSeq() const {
    return previous_seq_;
  }

  void
  onPressureCommit(uint64_t signal_seq) {
    previous_seq_ = signal_seq;
    staged_ = 0;
  }

private:
  uint64_t threshold_;
  uint64_t staged_ = 0;
  uint64_t previous_seq_ = 0;
};

/* ---- C4: recorded-work budget --------------------------------------------- */

/* Spec 07 clarification 1: 16 MiB of used bytes across the four pending
   vectors. */
constexpr uint64_t kRecordedWorkBudgetBytes = 16ull << 20;

/* After a batch is recorded: commit now? */
inline bool
RecordedWorkOverBudget(uint64_t recorded_bytes, bool enabled) {
  return enabled && recorded_bytes >= kRecordedWorkBudgetBytes;
}

/* What one batch kind normally needs: a pending vector reserves at most this
   many bytes when it is reset for the next batch. */
constexpr size_t kPendingReserveCapBytes = 64u << 10;

/* Elements to reserve for the next batch of a kind whose high-water mark (the
   largest count it held at a flush) is `peak` elements of `element_size` bytes:
   min(peak, 64 KiB / element size), no minimum. With the budget switched off
   (MADEIRA_D3D9_BATCH_BUDGET=0) the cap is off too: the whole high-water mark. */
inline size_t
PendingReserveCount(size_t peak, size_t element_size, bool budget_enabled = true) {
  if (!budget_enabled)
    return peak;
  return std::min(peak, kPendingReserveCapBytes / std::max<size_t>(1, element_size));
}

/* Before a batch is handed to the encoder: copy a vector into a tight one when
   it holds more than 64 KiB of capacity and is less than half full, or more
   than 4 KiB and less than a quarter full. */
inline bool
PendingShouldShrink(size_t used, size_t capacity, size_t element_size) {
  const uint64_t capacity_bytes = uint64_t(capacity) * element_size;
  if (capacity_bytes > (64u << 10) && uint64_t(used) * 2 < capacity)
    return true;
  return capacity_bytes > (4u << 10) && uint64_t(used) * 4 < capacity;
}

template <typename T>
inline void
ShrinkPendingVector(std::vector<T> &v) {
  if (!PendingShouldShrink(v.size(), v.capacity(), sizeof(T)))
    return;
  std::vector<T> tight;
  tight.reserve(v.size());
  for (auto &e : v)
    tight.push_back(std::move(e));
  v.swap(tight);
}

/* ---- C5: scene batching --------------------------------------------------- */

constexpr size_t kSceneBatchMaxOps = 256;

/* EndScene: drain the recorded work now? `recorded_ops` counts the pending
   operations (draws, blits, state references). A scene with nothing recorded
   (only clears, or nothing) takes the normal path. */
inline bool
EndSceneDrains(bool scene_batch, size_t recorded_ops) {
  if (!scene_batch || recorded_ops == 0)
    return true;
  return recorded_ops >= kSceneBatchMaxOps;
}

/* ---- C6: batched readbacks ------------------------------------------------ */

constexpr uint64_t kReadbackBatchMaxBytes = 16ull << 20;

/* The end (exclusive) of the readback batch that starts at `begin`: as many
   following entries as fit in kReadbackBatchMaxBytes together. An entry larger
   than the limit on its own forms a batch of one. Returns `begin` only when
   begin >= count. */
inline size_t
ReadbackBatchEnd(const uint64_t *sizes, size_t count, size_t begin) {
  if (begin >= count)
    return begin;
  uint64_t total = sizes[begin];
  size_t end = begin + 1;
  while (end < count && total + sizes[end] <= kReadbackBatchMaxBytes && sizes[end] <= kReadbackBatchMaxBytes) {
    total += sizes[end];
    ++end;
  }
  return end;
}

/* ---- C7: query polling ---------------------------------------------------- */

constexpr uint64_t kQueryPollRunGapNs = 250000; // 250 us
constexpr uint64_t kQueryParkNs = 100000;       // 100 us
/* Spec 07 clarification 2: the first two not-ready polls of a run return at
   once; the third and later ones park. */
constexpr uint32_t kQueryPollsBeforePark = 2;

/* Per-query polling state, reset by Issue(END). */
struct QueryPollState {
  uint64_t last_not_ready_ns = 0; // when the previous poll found the result not ready
  uint32_t run = 0;               // not-ready polls in the current run (adaptive)
  uint32_t polls_since_issue = 0; // every not-ready poll since Issue (non-adaptive)
  bool flushed_since_issue = false;

  void
  onIssueEnd() {
    *this = QueryPollState{};
  }
};

/* A poll of an EVENT or OCCLUSION query found the result not ready at `now_ns`
   (only such polls call this; a ready poll never reads the clock): park the
   caller before answering? Adaptive: the gap to the previous not-ready poll is
   measured start to start (so that poll's park counts in it); a gap of at most
   kQueryPollRunGapNs continues the run, a longer one starts a new run at 1.
   Not adaptive: the count is every not-ready poll since Issue. Either way,
   park once the count is more than kQueryPollsBeforePark. */
inline bool
QueryPollShouldPark(QueryPollState &state, uint64_t now_ns, bool adaptive) {
  state.polls_since_issue++;
  if (!adaptive)
    return state.polls_since_issue > kQueryPollsBeforePark;
  const bool continues =
      state.run != 0 && now_ns >= state.last_not_ready_ns && now_ns - state.last_not_ready_ns <= kQueryPollRunGapNs;
  state.run = continues ? state.run + 1 : 1;
  state.last_not_ready_ns = now_ns;
  return state.run > kQueryPollsBeforePark;
}

/* The forward-progress submit a poll asks for happens once per Issue(END):
   true the first time after each Issue(END). */
inline bool
QueryTakeFlush(QueryPollState &state) {
  if (state.flushed_since_issue)
    return false;
  state.flushed_since_issue = true;
  return true;
}

/* ---- C9: adapter modes ---------------------------------------------------- */

/* D3DFORMAT values (Microsoft's d3d9types.h): the display formats an app may
   enumerate modes for. */
constexpr uint32_t kD3DFmtX8R8G8B8 = 22;
constexpr uint32_t kD3DFmtR5G6B5 = 23;
constexpr uint32_t kD3DFmtX1R5G5B5 = 24;

inline bool
AdapterModeFormatListed(uint32_t d3d_format) {
  return d3d_format == kD3DFmtX8R8G8B8 || d3d_format == kD3DFmtR5G6B5 || d3d_format == kD3DFmtX1R5G5B5;
}

/* The modes reported for `d3d_format`: every display mode for a listed format
   (in the display list's order), none otherwise. */
template <typename Mode>
inline std::vector<Mode>
AdapterModesForFormat(uint32_t d3d_format, const std::vector<Mode> &display_modes) {
  if (!AdapterModeFormatListed(d3d_format))
    return {};
  return display_modes;
}

/* ---- C13: anisotropy limit ------------------------------------------------ */

/* DXMT_D9_ANISO_LIMIT: 1, 2, 4 or 8; anything else (or unset) is no cap (0). */
inline uint32_t
ParseAnisotropyLimit(const char *value) {
  const uint64_t n = ParseCount(value, 0);
  return (n == 1 || n == 2 || n == 4 || n == 8) ? uint32_t(n) : 0u;
}

inline uint32_t
CapAnisotropy(uint32_t requested, uint32_t limit) {
  return (limit != 0 && requested > limit) ? limit : requested;
}

/* ---- C15: small mirror cache ---------------------------------------------- */

constexpr uint64_t kSmallMirrorMaxBytes = 256u << 10; // per resource
constexpr uint64_t kSmallMirrorTotalBytes = 32u << 20; // all devices of the module

/* Bytes of mirrors kept by the cache. One per module (every device shares it). */
class MirrorCacheBudget {
public:
  /* Keep a mirror of `bytes`? Reserves the bytes when it says yes. */
  bool
  tryKeep(uint64_t bytes) {
    if (bytes == 0 || bytes > kSmallMirrorMaxBytes)
      return false;
    uint64_t used = used_.load(std::memory_order_relaxed);
    do {
      if (used + bytes > kSmallMirrorTotalBytes)
        return false;
    } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
  }

  /* A kept mirror is freed. */
  void
  release(uint64_t bytes) {
    used_.fetch_sub(bytes, std::memory_order_relaxed);
  }

  uint64_t
  used() const {
    return used_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> used_{0};
};

} // namespace dxmt::d9ios
