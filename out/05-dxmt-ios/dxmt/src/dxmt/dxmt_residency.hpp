#pragma once
// Changed for MYIOSDECK (spec 05 clarification 10: an LGPL-2.1-or-later block from steamac's
// DXMT patch 0023, marked inline, see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "winemetal.h"
#include "Metal.hpp"
#include "thread.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include "ftl.hpp"

namespace dxmt {

/* MYIOSDECK spec 05 clarification 56 (MIT): the residency set only makes resources resident;
   per-encoder useResource is also what lets Metal order a pass after one that wrote the same
   resource, and DXMT binds everything through argument buffers. So the set is opt-in: only
   DXMT_RESIDENCY_SET=1 turns it on. Returns nullptr for "use the set", else the reason the
   log line gives for useResource. */
inline const char *
ResidencySetOffReason(const char *value) {
  if (value && value[0] == '1' && value[1] == '\0')
    return nullptr;
  if (!value || !value[0])
    return "DXMT_RESIDENCY_SET unset";
  return "DXMT_RESIDENCY_SET not 1";
}

/**
LGPL-2.1-or-later, from steamac's DXMT patch 0023 (Jakey-Jakey): an `MTLResidencySet`
attached to the command queue, which replaces per-encoder `useResource`. Allocations are
added on first use, and committed right before the command buffer that uses them. Removal
(once freed) is committed immediately. MYIOSDECK spec 05 clarification 10: only on iOS 18
and later (CommandQueue decides); a handle is a WMT::Object here (MTLBuffer / MTLTexture).
*/
class ResidencySetTracker {
public:
  ResidencySetTracker(WMT::Reference<WMT::ResidencySet> &&set) : set_(std::move(set)) {}

  void
  incRef() {
    refcount_.fetch_add(1u, std::memory_order_acquire);
  }

  void
  decRef() {
    if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
      delete this;
  }

  void
  add(WMT::Object allocation) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    set_.addAllocations(&allocation.handle, 1);
    dirty_ = true;
  }

  void
  remove(WMT::Object allocation) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    set_.removeAllocations(&allocation.handle, 1);
    set_.commit();
    dirty_ = false;
  }

  void
  commit() {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (!dirty_)
      return;
    set_.commit();
    dirty_ = false;
  }

private:
  std::atomic<uint32_t> refcount_ = {0u};
  dxmt::mutex mutex_;
  WMT::Reference<WMT::ResidencySet> set_;
  bool dirty_ = false;
};
/* end of LGPL block */

enum DXMT_RESOURCE_RESIDENCY : uint32_t {
  DXMT_RESOURCE_RESIDENCY_NULL = 0,
  DXMT_RESOURCE_RESIDENCY_VERTEX_READ = 1 << 0,
  DXMT_RESOURCE_RESIDENCY_VERTEX_WRITE = 1 << 1,
  DXMT_RESOURCE_RESIDENCY_FRAGMENT_READ = 1 << 2,
  DXMT_RESOURCE_RESIDENCY_FRAGMENT_WRITE = 1 << 3,
  DXMT_RESOURCE_RESIDENCY_OBJECT_READ = 1 << 4,
  DXMT_RESOURCE_RESIDENCY_OBJECT_WRITE = 1 << 5,
  DXMT_RESOURCE_RESIDENCY_MESH_READ = 1 << 6,
  DXMT_RESOURCE_RESIDENCY_MESH_WRITE = 1 << 7,
  DXMT_RESOURCE_RESIDENCY_MESH_GS_READ = 1 << 8,
  DXMT_RESOURCE_RESIDENCY_MESH_GS_WRITE = 1 << 9,
  DXMT_RESOURCE_RESIDENCY_OBJECT_GS_READ = 1 << 10,
  DXMT_RESOURCE_RESIDENCY_OBJECT_GS_WRITE = 1 << 11,
  DXMT_RESOURCE_RESIDENCY_READ = DXMT_RESOURCE_RESIDENCY_VERTEX_READ | DXMT_RESOURCE_RESIDENCY_FRAGMENT_READ |
                                 DXMT_RESOURCE_RESIDENCY_OBJECT_READ | DXMT_RESOURCE_RESIDENCY_MESH_READ |
                                 DXMT_RESOURCE_RESIDENCY_MESH_GS_READ | DXMT_RESOURCE_RESIDENCY_OBJECT_GS_READ,
  DXMT_RESOURCE_RESIDENCY_WRITE = DXMT_RESOURCE_RESIDENCY_VERTEX_WRITE | DXMT_RESOURCE_RESIDENCY_FRAGMENT_WRITE |
                                  DXMT_RESOURCE_RESIDENCY_OBJECT_WRITE | DXMT_RESOURCE_RESIDENCY_MESH_WRITE |
                                  DXMT_RESOURCE_RESIDENCY_MESH_GS_WRITE | DXMT_RESOURCE_RESIDENCY_OBJECT_GS_WRITE,
};

struct DXMT_RESOURCE_RESIDENCY_STATE {
  uint64_t last_encoder_id = 0;
  DXMT_RESOURCE_RESIDENCY last_residency_mask = DXMT_RESOURCE_RESIDENCY_NULL;
};

constexpr bool
CheckResourceResidency(DXMT_RESOURCE_RESIDENCY_STATE &state, uint64_t encoder_id, DXMT_RESOURCE_RESIDENCY &requested) {
  if (encoder_id > state.last_encoder_id) {
    state.last_residency_mask = requested;
    state.last_encoder_id = encoder_id;
    requested = state.last_residency_mask;
    return true;
  }
  if (encoder_id == state.last_encoder_id) {
    if ((state.last_residency_mask & requested) == requested) {
      // it's already resident
      return false;
    }
    state.last_residency_mask = requested | state.last_residency_mask;
    requested = state.last_residency_mask;
    return true;
  }
  // invalid
  return false;
};

constexpr WMTResourceUsage
GetUsageFromResidencyMask(DXMT_RESOURCE_RESIDENCY mask) {
  return ((mask & DXMT_RESOURCE_RESIDENCY_READ) ? WMTResourceUsageRead : WMTResourceUsage(0)) |
         ((mask & DXMT_RESOURCE_RESIDENCY_WRITE) ? WMTResourceUsageWrite : WMTResourceUsage(0));
}

constexpr WMTRenderStages
GetStagesFromResidencyMask(DXMT_RESOURCE_RESIDENCY mask) {
  return ((mask & (DXMT_RESOURCE_RESIDENCY_FRAGMENT_READ | DXMT_RESOURCE_RESIDENCY_FRAGMENT_WRITE))
              ? WMTRenderStageFragment
              : WMTRenderStages(0)) |
         ((mask & (DXMT_RESOURCE_RESIDENCY_VERTEX_READ | DXMT_RESOURCE_RESIDENCY_VERTEX_WRITE))
              ? WMTRenderStageVertex
              : WMTRenderStages(0)) |
         ((mask & (DXMT_RESOURCE_RESIDENCY_OBJECT_READ | DXMT_RESOURCE_RESIDENCY_OBJECT_WRITE |
                   DXMT_RESOURCE_RESIDENCY_OBJECT_GS_READ | DXMT_RESOURCE_RESIDENCY_OBJECT_GS_WRITE))
              ? WMTRenderStageObject
              : WMTRenderStages(0)) |
         ((mask & (DXMT_RESOURCE_RESIDENCY_MESH_READ | DXMT_RESOURCE_RESIDENCY_MESH_WRITE |
                   DXMT_RESOURCE_RESIDENCY_MESH_GS_READ | DXMT_RESOURCE_RESIDENCY_MESH_GS_WRITE))
              ? WMTRenderStageMesh
              : WMTRenderStages(0));
}

} // namespace dxmt
