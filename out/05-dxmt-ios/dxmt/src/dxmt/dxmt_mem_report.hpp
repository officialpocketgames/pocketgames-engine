// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 90 (round 20): the [dxmt-mem] lines (dxmt_mem_report.cpp).
#pragma once

namespace dxmt::memledger {

/* Starts the process's report thread, once: it wakes every 250 ms and writes the lines
   ReportSchedule says are due. DXMT_MEM_LOG=0 leaves the lines out (the ledger still counts).
   Not in the ios_wow_native build, whose threads may not outlive a program (spec 33). */
void StartReport();

} // namespace dxmt::memledger
