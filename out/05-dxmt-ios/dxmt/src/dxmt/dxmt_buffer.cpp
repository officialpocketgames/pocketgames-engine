// Changed for MYIOSDECK (spec 06 B1; multi-slice allocations for dynamic-buffer discards). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "dxmt_buffer.hpp"
#include "dxmt_format.hpp"
#include "thread.hpp"
#include "util_bitness.hpp"
#include "util_likely.hpp"
#include "util_math.hpp"
#include "util_placed_memory.hpp"
#include "wsi_platform.hpp"
#include <cassert>
#include <mutex>

namespace dxmt {

std::atomic_uint64_t global_buffer_seq = {0};

BufferAllocation::BufferAllocation(
    WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, uint32_t min_slices
) :
    info_(info),
    flags_(flags) {
  // (sub)allocate a minimum of 256B buffer so that texture can be created
  info_.length = std::max<uint64_t>(info_.length, 256); // MYIOSDECK: portable to LP64 hosts (host benchmark)
  const uint64_t slice_length = info_.length;
  suballocation_size_ = info_.length;
  if (flags_.test(BufferAllocationFlag::SuballocateFromOnePage) && suballocation_size_ <= DXMT_PAGE_SIZE) {
    suballocation_size_ = align(info_.length, 16);
    suballocation_count_ = DXMT_PAGE_SIZE / suballocation_size_;
    info_.length = DXMT_PAGE_SIZE;
  }
  // MYIOSDECK (dynamic-buffer discard): an allocation of `min_slices` slices
  // of the buffer's length, 256-byte aligned, so a discard can move to the
  // next slice instead of needing a new allocation.
  if (min_slices > suballocation_count_) {
    suballocation_size_ = align(slice_length, 256);
    suballocation_count_ = min_slices;
    info_.length = uint64_t(suballocation_size_) * min_slices;
  }
  if (flags_.test(BufferAllocationFlag::CpuPlaced)) {
    // Page-aligned for newBufferWithBytesNoCopy: (16 KB pages on iOS)
    info_.length = PlacedMemoryLength(info_.length);
    placed_buffer = wsi::aligned_malloc(info_.length, kPlacedMemoryAlignment);
    info_.memory.set(placed_buffer);
  }
  obj_ = device.newBuffer(info_);
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = info_.memory.get_accessible_or_null();
  depkey = EncoderDepSet::generateNewKey(global_buffer_seq.fetch_add(1));
};

BufferAllocation::~BufferAllocation() {
  if (placed_buffer) {
    wsi::aligned_free(placed_buffer);
    placed_buffer = nullptr;
  }
}

WMT::Texture
Buffer::view(BufferViewKey key) {
  return view(key, current_.ptr());
};

WMT::Texture
Buffer::view(BufferViewKey key, BufferAllocation *allocation) {
  return view_(key, allocation).texture;
};

BufferView const &
Buffer::view_(BufferViewKey key) {
  return view_(key, current_.ptr());
};

BufferView const &
Buffer::view_(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return *allocation->cached_view_[key];
};

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key) {
  return residency(key, current_.ptr());
}

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return allocation->cached_view_[key]->residency;
}

void
Buffer::prepareAllocationViews(BufferAllocation *allocation) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  for (unsigned version = allocation->version_; version < version_; version++) {
    auto format = viewDescriptors_[version].format;
    auto texel_size = MTLGetTexelSize(format);
    assert(texel_size);
    assert(!(allocation->suballocation_size_ & (texel_size - 1)));
    auto total_length = allocation->suballocation_size_ * allocation->suballocation_count_;
    WMTTextureInfo info;
    info.type = WMTTextureTypeTextureBuffer;
    info.width = total_length / (uint64_t)texel_size;
    info.height = 1;
    info.depth = 1;
    info.array_length = 1;
    info.mipmap_level_count = 1;
    info.sample_count = 1;
    info.pixel_format = format;
    info.options = allocation->info_.options;
    auto usage = WMTTextureUsageShaderRead;
    if (!allocation->flags().test(BufferAllocationFlag::GpuReadonly) &&
       ( allocation->flags().test(BufferAllocationFlag::GpuManaged) ||  allocation->flags().test(BufferAllocationFlag::GpuPrivate))) {
      usage |= WMTTextureUsageShaderWrite;
      if (format == WMTPixelFormatR32Uint || format == WMTPixelFormatR32Sint ||
          (format == WMTPixelFormatRG32Uint && device_.supportsFamily(WMTGPUFamilyApple8))) {
        usage |= WMTTextureUsageShaderAtomic;
      }
    }
    info.usage = usage;

    auto view = allocation->obj_.newTexture(info, 0, total_length);

    allocation->cached_view_.push_back(std::make_unique<BufferView>(
        std::move(view), info.gpu_resource_id, allocation->suballocation_size_ / texel_size
    ));
  }
  allocation->version_ = version_;
};

BufferViewKey
Buffer::createView(BufferViewDescriptor const &descriptor) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  unsigned i = 0;
  for (; i < version_; i++) {
    if (viewDescriptors_[i].format == descriptor.format) {
      return i;
    }
  }
  viewDescriptors_.push_back(descriptor);
  version_ = version_ + 1;
  return i;
}

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, uint32_t min_slices) {
  WMTResourceOptions options = WMTResourceStorageModeShared;
  if (flags.test(BufferAllocationFlag::GpuReadonly)) {
    options |= WMTResourceHazardTrackingModeUntracked;
  }
  if (flags.test(BufferAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(BufferAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(BufferAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  // Spec 06 B1: in the i386 build every CPU-visible allocation brings its own
  // memory (from the DLL, so inside the program's window); memory Metal
  // allocates has no 32-bit address.
  if (kI386Build && !flags.test(BufferAllocationFlag::CpuInvisible)) {
    flags.set(BufferAllocationFlag::CpuPlaced);
  }
  WMTBufferInfo info;
  info.memory.set(0);
  info.length = length_;
  info.options = options;
  return new BufferAllocation(device_, info, flags, min_slices);
};

Rc<BufferAllocation>
Buffer::rename(Rc<BufferAllocation> &&newAllocation) {
  Rc<BufferAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void
Buffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Buffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

} // namespace dxmt