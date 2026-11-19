// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part A: one module, one bitness. True in an i386 build of the
// Windows half (32-bit programs through WoW64), false otherwise. Switches that
// spec 05 says are "on in spec 06" default to this.

#pragma once

namespace dxmt {

#if defined(__i386__) || defined(_M_IX86)
constexpr bool kI386Build = true;
#elif defined(DXMT_IOS_WOW_NATIVE)
/* Spec 33 B: the native frontend serves 32-bit programs, so their defaults apply. Where the
   i386 build places memory in the DLL because the program must address it, this build uses
   Part C's allocator for memory the program sees and lets Metal allocate the rest
   (kIosWowNative below). */
constexpr bool kI386Build = true;
#else
constexpr bool kI386Build = false;
#endif

/* Spec 33: the ios_wow_native build (native ARM64 code serving 32-bit programs). */
#ifdef DXMT_IOS_WOW_NATIVE
constexpr bool kIosWowNative = true;
#else
constexpr bool kIosWowNative = false;
#endif

} // namespace dxmt
