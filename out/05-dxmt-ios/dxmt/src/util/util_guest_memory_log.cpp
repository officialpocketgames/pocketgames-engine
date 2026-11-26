// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 round 2: the guest-memory lines through DXMT's logger (util_guest_memory.hpp). Apart
// from util_guest_memory.cpp so that a host test can link the ledger without the logger.

#include "util_guest_memory.hpp"

#include "log/log.hpp"

namespace dxmt::guest_memory {

void
Write(const std::string &line, bool error) {
  if (error)
    Logger::err(line);
  else
    Logger::info(line);
}

} // namespace dxmt::guest_memory
