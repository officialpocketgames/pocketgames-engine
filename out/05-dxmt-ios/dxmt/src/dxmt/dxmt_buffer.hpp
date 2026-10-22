#pragma once
// Changed for MYIOSDECK (spec 07 A4; multi-slice allocations for dynamic-buffer discards): contains an LGPL-2.1-or-later block from dxmt-d3d9
// v0.4-d3d9 (e8dd4c6), marked inline (see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT.

#include "Metal.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_residency.hpp"
#include "dxmt_allocation.hpp"
#include "rc/util_rc_ptr.hpp"
#include "thread.hpp"
#include "util_flags.hpp"

namespace dxmt {

enum class BufferAllocationFlag : uint32_t {
  GpuReadonly = 0,
  NoTracking = 0,
  GpuPrivate = 1,
  CpuInvisible = 1,
  CpuWriteCombined = 2,
  OwnedByCommandList = 3,
  GpuManaged = 4,
  /* will allocate at least one page of memory and try to suballocate from that */
  SuballocateFromOnePage = 5,
  CpuPlaced = 6,
};

typedef unsigned BufferViewKey;

struct BufferViewDescriptor {
  WMTPixelFormat format;
};

class Buffer;

struct BufferView {
  WMT::Reference<WMT::Texture> texture;
  uint64_t gpu_resource_id;
  uint64_t suballocation_texel;
  DXMT_RESOURCE_RESIDENCY_STATE residency{};

  BufferView(WMT::Reference<WMT::Texture> &&texture, uint64_t gpu_resource_id, uint64_t suballocation_texel) :
      texture(std::move(texture)),
      gpu_resource_id(gpu_resource_id),
      suballocation_texel(suballocation_texel) {}
};

class BufferAllocation : public Allocation {
  friend class Buffer;

public:

  WMT::Buffer
  buffer() const {
    return obj_;
  }

  Flags<BufferAllocationFlag>
  flags() const {
    return flags_;
  }

  void *
  mappedMemory(uint32_t sub) const noexcept {
    return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(mappedMemory_) + sub * suballocation_size_);
  }

  uint64_t
  gpuAddress() const noexcept {
    return gpuAddress_;
  }

  /* spec 07: LGPL-2.1-or-later, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6) */
  uint64_t
  length() const noexcept {
    return info_.length;
  }

  uint32_t
  currentSuballocation() const noexcept {
    return current_suballocation_;
  }
  /* end of LGPL block */

  void
  useSuballocation(uint32_t suballocation) noexcept {
    current_suballocation_ = suballocation;
  };

  bool
  hasSuballocatoin(uint32_t suballocation) const noexcept {
    return suballocation < suballocation_count_;
  }

  /* MYIOSDECK: number of slices (1 unless suballocated). */
  uint32_t
  suballocationCount() const noexcept {
    return suballocation_count_;
  }

  uint64_t
  currentSuballocationOffset() const noexcept {
    return current_suballocation_ * suballocation_size_;
  }

  uint64_t
  currentSuballocationOffset(uint64_t stride) const noexcept {
    return current_suballocation_ * stride;
  }

  void
  updateContents(uint64_t offset, const void *data, uint64_t length, uint32_t suballocation = 0) noexcept {
    if (likely(mappedMemory_ != nullptr && !flags_.test(BufferAllocationFlag::GpuManaged))) {
      memcpy(reinterpret_cast<char *>(mappedMemory_) + suballocation * suballocation_size_ + offset, data, length);
      return;
    }
    obj_.updateContents(suballocation * suballocation_size_ + offset, data, length);
  }

  DXMT_RESOURCE_RESIDENCY_STATE residencyState;
  EncoderDepKey depkey;

private:
  BufferAllocation(
      WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, uint32_t min_slices = 0
  );
  ~BufferAllocation();

  BufferAllocation(const BufferAllocation &) = delete;
  BufferAllocation(BufferAllocation &&) = delete;

  WMT::Reference<WMT::Buffer> obj_;
  WMTBufferInfo info_;
  uint32_t version_ = 0;
  Flags<BufferAllocationFlag> flags_;
  std::vector<std::unique_ptr<BufferView>> cached_view_;
  void *mappedMemory_;
  uint64_t gpuAddress_;
  uint32_t current_suballocation_ = 0;
  uint32_t suballocation_size_;
  uint32_t suballocation_count_ = 1;

  void * placed_buffer = nullptr;
};

class Buffer {
public:
  void incRef();
  void decRef();

  BufferViewKey createView(BufferViewDescriptor const &);

  constexpr BufferAllocation *
  current() {
    return current_.ptr();
  }

  /* MYIOSDECK: min_slices > 1 makes an allocation of that many slices (see the
     BufferAllocation constructor). */
  Rc<BufferAllocation> allocate(Flags<BufferAllocationFlag> flags, uint32_t min_slices = 0);

  Rc<BufferAllocation> rename(Rc<BufferAllocation> &&newAllocation);

  Buffer(uint64_t length, WMT::Device device) : length_(length), device_(device) {}

  WMT::Texture view(BufferViewKey key);
  WMT::Texture view(BufferViewKey key, BufferAllocation *allocation);

  BufferView const &view_(BufferViewKey key); 
  BufferView const &view_(BufferViewKey key, BufferAllocation *allocation);

  DXMT_RESOURCE_RESIDENCY_STATE &residency(BufferViewKey key);
  DXMT_RESOURCE_RESIDENCY_STATE &residency(BufferViewKey key, BufferAllocation *allocation);

  constexpr uint64_t
  length() {
    return length_;
  };

  WMTPixelFormat pixelFormat(BufferViewKey view) const {
    return viewDescriptors_[view].format;
  }

private:
  void prepareAllocationViews(BufferAllocation *allocation);

  uint64_t length_;

  Rc<BufferAllocation> current_;
  uint32_t version_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};

  std::vector<BufferViewDescriptor> viewDescriptors_;
  dxmt::mutex mutex_;
  WMT::Device device_;
};

struct BufferSlice {
  uint32_t byteOffset = 0;
  uint32_t byteLength = 0;
  uint32_t firstElement = 0;
  uint32_t elementCount = 0;
};

} // namespace dxmt