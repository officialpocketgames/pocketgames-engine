// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part A: one module, one bitness. True in an i386 build of the
// Windows half (32-bit programs through WoW64), false otherwise. Switches that
// spec 05 says are "on in spec 06" default to this.

#pragma once

namespace dxmt {

#if defined(__i386__) || defined(_M_IX86)
constexpr bool kI386Build = true;
#else
constexpr bool kI386Build = false;
#endif

} // namespace dxmt
