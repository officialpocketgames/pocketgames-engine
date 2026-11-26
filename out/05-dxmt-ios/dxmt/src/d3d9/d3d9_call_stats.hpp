// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 3: call statistics, switch DXMT_D9_CALL_STATS (Part C's
// rule: unset, empty or 0 is off, anything else on; default off).
//
// Every public method of every interface the frontend hands out, and every
// export of d3d9.dll, starts with one of the macros below, before anything else
// (d3d9_call_list.h lists the pairs; tests/check-d3d9-call-sites.sh checks that
// each one is counted exactly once, at its method's entry). With the switch off
// a method pays one test of a flag read once when the DLL loads. With it on it
// adds 1 to its pair's counter (a plain add on a static table), and 1 to the
// other-threads counter when the calling thread isn't the one that created the
// device. The first Present that comes 10 s or more after the period started
// logs one info line and resets every counter (d3d9_call_stats.cpp).

#pragma once

#include "d3d9_call_stats_table.hpp"
#include "d3d9_fail_log.hpp" // spec 07 clarification 5
#include "util_likely.hpp"

namespace dxmt::d9cs {

/* DXMT_D9_CALL_STATS, read once when the DLL loads. */
extern bool g_enabled;

/* Count a call of pair `id`. */
void NoteCall(unsigned id);
/* Count a Present (any interface's Present, or PresentEx). First, if the
   period is 10 s or older, log its report and start the next one: the
   Present that triggers a report belongs to the new period. */
void NotePresent(unsigned id);
/* Count a call of an export. The first counted call starts the first period,
   and nothing reaches an interface before an export has handed it out. */
void NoteExport(unsigned id);
/* A device is being created on this thread: from now on calls from any other
   thread are also counted apart. With several devices, the newest one's
   thread. */
void NoteDeviceThread();

} // namespace dxmt::d9cs

#define D9_CALL(label, method)                                                                                         \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d9cs::g_enabled))                                                                             \
      ::dxmt::d9cs::NoteCall(::dxmt::d9cs::D9C_##label##_##method);                                                    \
  } while (0)

/* Clarification 7: a Present also writes the failed-call repeat lines when they are due (one
   relaxed load when none waits), in every build. */
#define D9_CALL_PRESENT(label, method)                                                                                 \
  do {                                                                                                                 \
    ::dxmt::d9fail::Tick();                                                                                            \
    if (unlikely(::dxmt::d9cs::g_enabled))                                                                             \
      ::dxmt::d9cs::NotePresent(::dxmt::d9cs::D9C_##label##_##method);                                                 \
  } while (0)

/* Spec 07 clarification 5: a covered method's body runs in a lambda; a failing result, or a
   success that left the out-pointer null, is logged once per (method, result), at most 64 lines
   (d3d9_fail_log.hpp). Written as
     D9_CALL(label, method);
     return D9_CHECKED_BEGIN { ...the body... } D9_CHECKED_END(label, method, null_out, args);
   `null_out` is evaluated only after a success; `args` (a std::string) only for a failure that
   isn't expected. A success with its out-pointer set costs one branch. Clarification 7: a pair
   that fails again is counted, and the counts are written every 10 s (Tick). */
namespace dxmt::d9fail {
Limiter &ProcessLimiter();
void Write(const std::string &line);
/* The frontend's monotonic clock, in ns (never 0). */
uint64_t NowNs();
/* Clarification 7: write the repeat lines if they are due. */
void Tick();

template <typename Body, typename NullOut, typename Args>
inline auto
Checked(Body &&body, unsigned method, NullOut &&null_out, Args &&args) {
  auto hr = body();
  const bool failed = int32_t(hr) < 0;
  const bool null = !failed && null_out();
  if (likely(!failed && !null))
    return hr;
  std::string line =
      Consider(ProcessLimiter(), method, ::dxmt::d9cs::kD9CallNames[method], int32_t(hr), null, args, NowNs());
  if (!line.empty())
    Write(line);
  Tick();
  return hr;
}
} // namespace dxmt::d9fail

#define D9_CHECKED_BEGIN ::dxmt::d9fail::Checked([&]() -> HRESULT
#define D9_CHECKED_END(label, method, null_out, args)                                                                  \
  , ::dxmt::d9cs::D9C_##label##_##method, [&]() -> bool { return (null_out); },                                       \
      [&]() -> std::string { return (args); })

#define D9_CALL_EXPORT(name)                                                                                           \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d9cs::g_enabled))                                                                             \
      ::dxmt::d9cs::NoteExport(::dxmt::d9cs::D9C_d3d9_##name);                                                         \
  } while (0)
