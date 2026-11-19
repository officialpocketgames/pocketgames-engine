// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 Part B: stand-ins for Darwin-only APIs, so the ios_wow_native build compiles on Linux
// (compile-native-linux.sh includes this after util_ios_wow_native.h). Never part of the iOS
// build.

#pragma once

#if defined(__linux__) && defined(__cplusplus)
#include <pthread.h>

/* Darwin names only the calling thread (util_env.cpp's setThreadName). */
inline int
pthread_setname_np(const char *name) {
  return pthread_setname_np(pthread_self(), name);
}
#endif
