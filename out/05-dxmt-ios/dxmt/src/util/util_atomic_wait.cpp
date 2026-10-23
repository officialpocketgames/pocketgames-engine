// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part D: the start-up log line of util_atomic_wait.hpp.

#include "util_atomic_wait.hpp"
#include "log/log.hpp"

namespace dxmt {

void
atomic_wait_log_backend() {
#ifdef DXMT_ATOMIC_WAIT_NTDLL
  Logger::info("atomic wait: ntdll RtlWaitOnAddress / RtlWakeAddress*");
#else
  Logger::info("atomic wait: std::atomic wait / notify");
#endif
}

} // namespace dxmt
