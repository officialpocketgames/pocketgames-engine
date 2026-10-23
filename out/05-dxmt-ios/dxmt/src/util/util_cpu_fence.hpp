#pragma once
// Changed for MYIOSDECK (spec 06 D: waits go through util_atomic_wait.hpp).
// Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include <atomic>
#include <cstdint>
#include "util_atomic_wait.hpp"

namespace dxmt {

class CpuFence {
public:
  void wait(uint64_t value) {
    auto current = value_.load(std::memory_order_acquire);
    while (current < value) {
      atomic_wait(value_, current);
      current = value_.load(std::memory_order_acquire);
    }
  }

  void signal(uint64_t value) {
    auto current = value_.load(std::memory_order_relaxed);
    do {
      if (value <= current)
        return;
    } while (!value_.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed));
    atomic_notify_all(value_);
  }

  uint64_t signaledValue() {
    return value_.load(std::memory_order_acquire);
  }

  CpuFence(): value_(0) {}
  CpuFence(uint64_t initial_value): value_(initial_value) {}

private:
  std::atomic<uint64_t> value_;
};
} // namespace dxmt
