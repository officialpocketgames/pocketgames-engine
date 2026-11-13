// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 63: Direct3D 11 call statistics, switch DXMT_D11_CALL_STATS (unset,
// empty or 0 off; 1 counts; 2 counts and times; anything else off with one warning). Test
// builds only: nothing in the store build sets it.
//
// Every public method of the device context (immediate and deferred, every interface version),
// of the device (every version) and the swap chain's Presents starts with one of the macros
// below, before anything else (d3d11_call_list.h lists them; tests/check-d3d11-call-sites.sh
// checks that each is counted once, at its method's entry). AddRef, Release and QueryInterface
// of the device's other objects go into three shared counters. With the switch off a method
// pays one test of a flag read once when the DLL loads. With it on it adds 1 to its counter (a
// plain add on a static table); at 2 a device-context call also reads the counter register on
// entry and return and adds the difference to its method. The first Present 10 s or more after
// the period started logs the report and resets every counter (d3d11_call_stats.cpp).

#pragma once

#include "d3d11_call_stats_table.hpp"
#include "util_likely.hpp"

#include <chrono>
#include <cstdint>

namespace dxmt::d11cs {

/* DXMT_D11_CALL_STATS's level, read once when the DLL loads. */
extern unsigned g_level;

void NoteCall(unsigned id);
/* Count a Present; first, when the period is 10 s or older, log its report and start the next
   one (the Present that triggers it belongs to the new period). */
void NotePresent(unsigned id);
/* The device is being made on this thread: calls from any other thread are also counted apart. */
void NoteDeviceThread();
/* Level 2: a context call's time, in counter ticks. */
void AddTicks(unsigned id, uint64_t ticks);
/* The work counters (only while g_level != 0). */
D11Work &Work();

/* The cheapest monotonic counter this build has: CNTVCT_EL0 on arm64 and arm64ec builds (the
   generic timer, 24 MHz on these phones, about 42 ns a tick); steady_clock's nanoseconds
   elsewhere. */
inline uint64_t
ReadTicks() {
#if defined(__aarch64__) || defined(__arm64ec__)
  uint64_t value;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
  return value;
#else
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
  );
#endif
}

inline uint64_t
TickHz() {
#if defined(__aarch64__) || defined(__arm64ec__)
  uint64_t value;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
  return value ? value : 24000000;
#else
  return 1000000000ull;
#endif
}

/* The device-context call running on this thread (its id), while the switch is on: a state
   call that sets what was set is counted against it. */
inline thread_local unsigned t_current_call = kD11CallCount;

/* A device-context call: counted on entry, timed (level 2) until its scope ends. */
class ContextCall {
public:
  explicit ContextCall(unsigned id) {
    if (unlikely(g_level)) {
      NoteCall(id);
      active_ = true;
      id_ = id;
      previous_ = t_current_call;
      t_current_call = id;
      if (g_level >= 2)
        start_ = ReadTicks();
    }
  }
  ~ContextCall() {
    if (unlikely(active_)) {
      if (start_)
        AddTicks(id_, ReadTicks() - start_);
      t_current_call = previous_;
    }
  }
  ContextCall(const ContextCall &) = delete;
  ContextCall &operator=(const ContextCall &) = delete;

private:
  bool active_ = false;
  unsigned id_ = 0;
  unsigned previous_ = kD11CallCount;
  uint64_t start_ = 0;
};

/* A state call found the value already set: counted against the running context method. */
inline void
NoteRepeated() {
  if (unlikely(g_level)) {
    unsigned method = D11ContextMethodOf(t_current_call);
    if (method < kD11ContextMethods)
      Work().repeated[method]++;
  }
}

/* Work counted where it happens: `field` of the work counters, when the switch is on. */
#define D11_WORK(expr)                                                                                                 \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d11cs::g_level)) {                                                                            \
      auto &work_ = ::dxmt::d11cs::Work();                                                                             \
      (void)work_;                                                                                                     \
      expr;                                                                                                            \
    }                                                                                                                  \
  } while (0)

} // namespace dxmt::d11cs

/* In a device-context method: kD11csBase is the context's range (immediate or deferred). */
#define D11_CTX_CALL(method)                                                                                           \
  ::dxmt::d11cs::ContextCall d11cs_call_(this->kD11csBase + ::dxmt::d11cs::D11X_##method)

#define D11_DEVICE_CALL(method)                                                                                        \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d11cs::g_level))                                                                              \
      ::dxmt::d11cs::NoteCall(::dxmt::d11cs::kD11IdDevice + ::dxmt::d11cs::D11D_##method);                             \
  } while (0)

#define D11_CHILD_CALL(method)                                                                                         \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d11cs::g_level))                                                                              \
      ::dxmt::d11cs::NoteCall(::dxmt::d11cs::kD11IdOther + ::dxmt::d11cs::D11O_Child_##method);                        \
  } while (0)

#define D11_PRESENT_CALL(method)                                                                                       \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d11cs::g_level))                                                                              \
      ::dxmt::d11cs::NotePresent(::dxmt::d11cs::kD11IdOther + ::dxmt::d11cs::D11O_SwapChain_##method);                 \
  } while (0)
