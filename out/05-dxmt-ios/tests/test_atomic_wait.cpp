// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part D: host test of the generic (std::atomic) backend of
// dxmt::atomic_wait / atomic_notify_one / atomic_notify_all
// (dxmt/src/util/util_atomic_wait.hpp).
//
//   c++ -std=c++20 -pthread -I../dxmt/src/util test_atomic_wait.cpp -o test_atomic_wait

#include "test_common.hpp"

#define DXMT_ATOMIC_WAIT_GENERIC 1
#include "util_atomic_wait.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace dxmt {
int g_backend_logs = 0;
void
atomic_wait_log_backend() {
  g_backend_logs++;
}
} // namespace dxmt

namespace {

using namespace std::chrono_literals;

void
TestWaiterWakesOnNotify() {
  std::atomic<uint64_t> value{1};
  std::atomic<bool> woke{false};
  std::thread waiter([&] {
    dxmt::atomic_wait(value, 1, std::memory_order_acquire);
    woke.store(true);
  });
  std::this_thread::sleep_for(50ms);
  CHECK(!woke.load()); // still waiting while the value is unchanged
  value.store(2, std::memory_order_release);
  dxmt::atomic_notify_one(value);
  waiter.join(); // returns once woken
  CHECK(woke.load());
}

void
TestNotifyAllWakesEveryWaiter() {
  std::atomic<bool> ready{false};
  std::atomic<int> woke{0};
  std::thread a([&] {
    dxmt::atomic_wait(ready, false);
    woke++;
  });
  std::thread b([&] {
    dxmt::atomic_wait(ready, false);
    woke++;
  });
  std::this_thread::sleep_for(50ms);
  ready.store(true);
  dxmt::atomic_notify_all(ready);
  a.join();
  b.join();
  CHECK_EQ(woke.load(), 2);
}

void
TestChangedValueReturnsAtOnce() {
  std::atomic<uint64_t> value{5};
  auto t0 = std::chrono::steady_clock::now();
  dxmt::atomic_wait(value, 4); // value != old: no wait at all
  auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(elapsed < 1s);

  // The old value may have a different integer type than the atomic.
  std::atomic<uint64_t> chunks{3};
  dxmt::atomic_wait(chunks, 2, std::memory_order_acquire);
  CHECK(true);
}

void
TestBackendLoggedOnce() {
  CHECK_EQ(dxmt::g_backend_logs, 1);
}

} // namespace

int
main() {
  TestWaiterWakesOnNotify();
  TestNotifyAllWakesEveryWaiter();
  TestChangedValueReturnsAtOnce();
  TestBackendLoggedOnce();
  return TestSummary("test_atomic_wait");
}
