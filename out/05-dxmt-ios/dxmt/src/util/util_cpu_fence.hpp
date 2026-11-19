#pragma once
// Changed for MYIOSDECK (spec 06 D: waits go through util_atomic_wait.hpp; spec 05 round 18:
// waitBounded and waitFor, the bounded waits). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
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

  /* MYIOSDECK spec 05 H2 (moved here from CommandQueue::WaitCPUFenceBounded in round 18): true
     at once if the value has reached `value`; otherwise spin 64 reloads, then yield the thread
     until the deadline, re-checking after each yield. Returns whether `value` was reached;
     never waits longer than `timeout_ns`. */
  bool waitBounded(uint64_t value, uint64_t timeout_ns) {
    if (value_.load(std::memory_order_acquire) >= value)
      return true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
    for (unsigned i = 0; i < 64; i++) {
      if (value_.load(std::memory_order_acquire) >= value)
        return true;
    }
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
      if (value_.load(std::memory_order_acquire) >= value)
        return true;
    }
    return value_.load(std::memory_order_acquire) >= value;
  }

  /* MYIOSDECK spec 05 round 18 (item 80): a blocking wait until the value reaches `value` or
     `timeout_ns` pass; returns whether it was reached. The ntdll backend (the Windows half)
     sleeps in RtlWaitOnAddress with a timeout, so the core is given up and signal()'s wake-up
     ends the wait; a build without that wait (native, the unix half, host tests) uses
     waitBounded's loop. */
  bool waitFor(uint64_t value, uint64_t timeout_ns) {
#ifdef DXMT_ATOMIC_WAIT_NTDLL
    auto current = value_.load(std::memory_order_acquire);
    if (current >= value)
      return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
    while (current < value) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        return false;
      atomic_wait_timeout(
          value_, current, (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count(),
          std::memory_order_acquire
      );
      current = value_.load(std::memory_order_acquire);
    }
    return true;
#else
    return waitBounded(value, timeout_ns);
#endif
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
