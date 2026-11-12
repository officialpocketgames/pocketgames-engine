// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 57: PG_DXMT_SERIALIZE, a test switch that takes the GPU's overlap away
// step by step. Each level includes the ones below it:
//   1  passes in recorded order: the encoder optimiser neither merges nor moves anything;
//   2  one frame on the GPU at a time: Present waits for its own chunk;
//   3  no CPU write while the GPU runs: commit and wait for every committed chunk before DXMT
//      hands the CPU memory to write (Map, UpdateSubresource, D3D9 Lock);
//   4  every pass after the previous one: an MTLFence between consecutive encoders.
// Plain C++ with no Metal: the queue, the contexts and the host tests share it.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace dxmt {

constexpr unsigned kSerializeMaxLevel = 4;
constexpr uint64_t kSerializeReportPresents = 600;

/* The switch's value: unset (null), empty or "0" is off (0); "1"-"4" is that level. Anything
   else is off too, with `ignored` set so the device can say so once. */
struct SerializeSetting {
  unsigned level = 0;
  bool ignored = false;
};

inline SerializeSetting
SerializeSettingFromValue(const char *value) {
  SerializeSetting setting;
  if (!value || !value[0] || (value[0] == '0' && !value[1]))
    return setting;
  if (value[0] >= '1' && value[0] <= '0' + int(kSerializeMaxLevel) && !value[1]) {
    setting.level = unsigned(value[0] - '0');
    return setting;
  }
  setting.ignored = true;
  return setting;
}

/* "dxmt: serialize level 3 (test switch)" */
inline std::string
SerializeLevelLine(unsigned level) {
  return "dxmt: serialize level " + std::to_string(level) + " (test switch)";
}

constexpr bool
SerializeKeepsEncoderOrder(unsigned level) {
  return level >= 1;
}

constexpr bool
SerializeWaitsAfterPresent(unsigned level) {
  return level >= 2;
}

constexpr bool
SerializeWaitsBeforeCpuWrite(unsigned level) {
  return level >= 3;
}

constexpr bool
SerializeFencesEncoders(unsigned level) {
  return level >= 4;
}

/* The waits the levels asked for, reported every 600 presents. Waits come from the API threads,
   presents from the presenting one. */
class SerializeStats {
public:
  void
  addWait(uint64_t ns) {
    waits_.fetch_add(1, std::memory_order_relaxed);
    wait_ns_.fetch_add(ns, std::memory_order_relaxed);
  }

  /* Called once per present. Every 600th, fills `line` with "dxmt: serialize: <w> waits, <ms>
     ms waited in the last 600 presents", starts the next 600 and returns true. */
  bool
  present(std::string &line) {
    if (++presents_ < kSerializeReportPresents)
      return false;
    presents_ = 0;
    uint64_t waits = waits_.exchange(0, std::memory_order_relaxed);
    uint64_t ms = wait_ns_.exchange(0, std::memory_order_relaxed) / 1000000u;
    line = "dxmt: serialize: " + std::to_string(waits) + " waits, " + std::to_string(ms) +
           " ms waited in the last " + std::to_string(kSerializeReportPresents) + " presents";
    return true;
  }

  uint64_t
  waits() const {
    return waits_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> waits_{0};
  std::atomic<uint64_t> wait_ns_{0};
  uint64_t presents_ = 0;
};

/* Waits until the queue's GPU-completed sequence reaches `seq`; counts the wait when there was
   one. `Queue` has CoherentSeqId() and WaitCPUFence(seq) (CommandQueue, or a fake). */
template <typename Queue>
bool
SerializeWaitFor(Queue &queue, SerializeStats &stats, uint64_t seq) {
  if (!seq || queue.CoherentSeqId() >= seq)
    return false;
  auto t0 = std::chrono::steady_clock::now();
  queue.WaitCPUFence(seq);
  auto t1 = std::chrono::steady_clock::now();
  stats.addWait(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  return true;
}

/* Level 2, right after the present's chunk was committed (the chunk before the one now being
   recorded): wait until the GPU has finished it. */
template <typename Queue>
bool
SerializeAfterPresent(Queue &queue, SerializeStats &stats, unsigned level) {
  if (!SerializeWaitsAfterPresent(level))
    return false;
  uint64_t current = queue.CurrentSeqId();
  return current && SerializeWaitFor(queue, stats, current - 1);
}

/* Level 3, before the CPU gets memory to write: `commit()` commits the chunk being recorded,
   then wait until the GPU has finished every committed chunk. */
template <typename Queue, typename Commit>
bool
SerializeBeforeCpuWrite(Queue &queue, SerializeStats &stats, unsigned level, Commit &&commit) {
  if (!SerializeWaitsBeforeCpuWrite(level))
    return false;
  commit();
  uint64_t current = queue.CurrentSeqId();
  return current && SerializeWaitFor(queue, stats, current - 1);
}

/* The encoder optimiser's loop, from DXMT's flushCommands (upstream a3a6e8f, MIT), as a template
   so a host test can drive it: from the second-last encoder backwards, each is compared with
   the ones after it (at most `threshold` apart) until the relation says to synchronise. The
   relation merges or moves encoders itself; `is_null` skips emptied ones. */
template <typename Encoder, typename Relation, typename IsNull>
void
OptimizeEncoderList(Encoder **encoders, unsigned count, unsigned threshold, Relation &&synchronize, IsNull &&is_null) {
  if (count <= 1)
    return;
  for (unsigned j = count - 2; j != ~0u; j--) {
    if (is_null(encoders[j]))
      continue;
    for (unsigned i = j + 1; i < count && i < j + threshold; i++) {
      if (is_null(encoders[i]))
        continue;
      if (synchronize(encoders[j], encoders[i]))
        break;
    }
  }
}

} // namespace dxmt
