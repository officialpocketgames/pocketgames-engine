// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 23: which WMTComputePipelineInfo.max_total_threads_per_threadgroup
// values reach MTLComputePipelineDescriptor.maxTotalThreadsPerThreadgroup. Only 1-1024
// (Direct3D's numthreads limit and every Apple GPU's per-threadgroup limit); anything else
// would make Metal assert, so it is ignored. Plain C, also compiles as C++, for the host tests.

#ifndef DXMT_COMPUTE_PIPELINE_LIMITS_H
#define DXMT_COMPUTE_PIPELINE_LIMITS_H

#include <stdint.h>

#define DXMT_MAX_THREADS_PER_THREADGROUP 1024u

enum dxmt_threads_per_threadgroup_use {
  DXMT_THREADS_DEFAULT = 0, /* 0: nothing asked, Metal's default */
  DXMT_THREADS_APPLY = 1,   /* 1-1024: set on the descriptor */
  DXMT_THREADS_IGNORED = 2, /* anything else: Metal's default, and the log line */
};

static inline enum dxmt_threads_per_threadgroup_use
dxmt_threads_per_threadgroup_use(uint32_t n) {
  if (n == 0)
    return DXMT_THREADS_DEFAULT;
  return n <= DXMT_MAX_THREADS_PER_THREADGROUP ? DXMT_THREADS_APPLY : DXMT_THREADS_IGNORED;
}

#endif
