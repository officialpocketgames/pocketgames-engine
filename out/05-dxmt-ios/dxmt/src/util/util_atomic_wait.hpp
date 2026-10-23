// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part D: waiting on another thread. DXMT's threads hand work to each
// other with std::atomic<T>::wait / notify_one / notify_all. In the mingw
// i386 C++ runtime those depend on an OS entry point looked up at run time,
// and under the app's Wine that lookup can fail silently. These replacements
// call ntdll directly on a Windows host and forward to std::atomic elsewhere
// (the unix half, native builds, host unit tests).
//
// Define DXMT_ATOMIC_WAIT_GENERIC to force the std::atomic backend.

#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

#if defined(_WIN32) && !defined(DXMT_NATIVE) && !defined(DXMT_ATOMIC_WAIT_GENERIC)
#define DXMT_ATOMIC_WAIT_NTDLL 1
#endif

#ifdef DXMT_ATOMIC_WAIT_NTDLL
// Declared here rather than taken from a header; linked from ntdll (-lntdll).
extern "C" {
__declspec(dllimport) long __stdcall RtlWaitOnAddress(
    const volatile void *address, const void *compare_address, size_t address_size, const long long *timeout
);
__declspec(dllimport) void __stdcall RtlWakeAddressSingle(const void *address);
__declspec(dllimport) void __stdcall RtlWakeAddressAll(const void *address);
}
#endif

namespace dxmt {

/* Logs once which backend is in use (util_atomic_wait.cpp). */
void atomic_wait_log_backend();

namespace detail {
inline void
atomic_wait_note() {
  static const bool logged = (atomic_wait_log_backend(), true);
  (void)logged;
}
} // namespace detail

/* Blocks while `atomic` still holds `old`. Wake-ups can be spurious, so the
   value is checked again after each one. */
template <typename T>
inline void
atomic_wait(
    const std::atomic<T> &atomic, std::type_identity_t<T> old, std::memory_order order = std::memory_order_seq_cst
) {
  detail::atomic_wait_note();
#ifdef DXMT_ATOMIC_WAIT_NTDLL
  static_assert(sizeof(std::atomic<T>) == sizeof(T), "the atomic must be the value itself");
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8, "unsupported size");
  while (atomic.load(order) == old)
    RtlWaitOnAddress(&atomic, &old, sizeof(T), nullptr);
#else
  atomic.wait(old, order);
#endif
}

template <typename T>
inline void
atomic_notify_one(std::atomic<T> &atomic) {
  detail::atomic_wait_note();
#ifdef DXMT_ATOMIC_WAIT_NTDLL
  RtlWakeAddressSingle(&atomic);
#else
  atomic.notify_one();
#endif
}

template <typename T>
inline void
atomic_notify_all(std::atomic<T> &atomic) {
  detail::atomic_wait_note();
#ifdef DXMT_ATOMIC_WAIT_NTDLL
  RtlWakeAddressAll(&atomic);
#else
  atomic.notify_all();
#endif
}

} // namespace dxmt
