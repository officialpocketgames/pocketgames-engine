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

#define D9_CALL_PRESENT(label, method)                                                                                 \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d9cs::g_enabled))                                                                             \
      ::dxmt::d9cs::NotePresent(::dxmt::d9cs::D9C_##label##_##method);                                                 \
  } while (0)

#define D9_CALL_EXPORT(name)                                                                                           \
  do {                                                                                                                 \
    if (unlikely(::dxmt::d9cs::g_enabled))                                                                             \
      ::dxmt::d9cs::NoteExport(::dxmt::d9cs::D9C_d3d9_##name);                                                         \
  } while (0)
