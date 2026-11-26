// Changed for MYIOSDECK (spec 05 D5, H7; spec 06 B). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
// Changed for MYIOSDECK (spec 33 B2: buffer-backed textures of the ios_wow_native build from Part C).
// Changed for MYIOSDECK (spec 06 round 2: placed memory counted as guest memory, its failure seen).
// Changed for MYIOSDECK (spec 05 item 90: each allocation in the memory ledger; the depth rename
// pool bounded).
#include "dxmt_texture.hpp"
#include "dxmt_bc_emulation.hpp"
#include "dxmt_mem_track.hpp"
#include "log/log.hpp"
#include "util_ios_switch.hpp"
#include "util_string.hpp"
#include <algorithm>
#include "util_guest_memory.hpp"
#include "dxmt_residency.hpp"
#include "util_placed_memory.hpp"
#include "wsi_platform.hpp"
#include <atomic>
#include <cassert>
#ifdef DXMT_IOS
#include "dxmt_mip_clamp.hpp"
#endif

namespace dxmt {

std::atomic_uint64_t global_texture_seq = {0};

void
TextureView::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
TextureView::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

TextureView::TextureView(TextureAllocation *allocation) :
    texture(allocation->texture()),
    gpuResourceID(allocation->gpuResourceID),
    allocation(allocation),
    key(0) {}

TextureView::TextureView(TextureAllocation *allocation, TextureViewKey key, TextureViewDescriptor descriptor) :
    gpuResourceID(0),
    allocation(allocation),
    key(key) {
  auto parent = allocation->texture();
#ifdef DXMT_IOS
  WMTTextureSwizzleChannels swizzle = descriptor.swizzle; // spec 05 H7
#else
  WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha
  };
#endif
  texture = parent.newTextureView(
      descriptor.format, descriptor.type, descriptor.firstMiplevel, descriptor.miplevelCount,
      descriptor.firstArraySlice, descriptor.arraySize, swizzle, gpuResourceID
  );
}

TextureAllocation::TextureAllocation(
    Texture *descriptor, WMT::Reference<WMT::Buffer> &&buffer, void *mapped_buffer, const WMTTextureInfo &info,
    unsigned bytes_per_row, Flags<TextureAllocationFlag> flags
) :
    descriptor(descriptor),
    mappedMemory(mapped_buffer),
    buffer_(std::move(buffer)),
    flags_(flags) {
  auto info_copy = info;
  obj_ = buffer_.newTexture(info_copy, 0, bytes_per_row);

  gpuResourceID = info_copy.gpu_resource_id;
  machPort = 0;
  depkey = EncoderDepSet::generateNewKey(global_texture_seq.fetch_add(1));
};

TextureAllocation::TextureAllocation(
    Texture *descriptor, WMT::Reference<WMT::Texture> &&texture, const WMTTextureInfo &textureDescriptor,
    Flags<TextureAllocationFlag> flags
) :
    descriptor(descriptor),
    obj_(std::move(texture)),
    flags_(flags) {
  mappedMemory = nullptr;
  gpuResourceID = textureDescriptor.gpu_resource_id;
  machPort = textureDescriptor.mach_port;
  depkey = EncoderDepSet::generateNewKey(global_texture_seq.fetch_add(1));
};

TextureAllocation::~TextureAllocation(){
#if defined(__i386__) || defined(DXMT_IOS_WOW_NATIVE) /* MYIOSDECK spec 33 B2 */
  guest_memory::Free(mappedMemory, placedLength, guest_memory::Use::Maps); // spec 06 round 2
#endif
};

void
Texture::prepareAllocationViews(TextureAllocation *allocaiton) {
  if (allocaiton->version_ < 1) {
    allocaiton->cached_view_.push_back(new TextureView(allocaiton));
    allocaiton->version_ = 1;
  }
  std::shared_lock<dxmt::shared_mutex> lock(mutex_);
  for (unsigned version = allocaiton->version_; version < version_; version++) {
    allocaiton->cached_view_.push_back(new TextureView(allocaiton, version, viewDescriptors_[version]));
  }
  allocaiton->version_ = version_;
}

TextureViewKey
Texture::createView(TextureViewDescriptor const &descriptor) {
  std::unique_lock<dxmt::shared_mutex> lock(mutex_);
  unsigned i = 0;
  for (; i < version_; i++) {
    if (viewDescriptors_[i].format != descriptor.format)
      continue;
    if (viewDescriptors_[i].type != descriptor.type)
      continue;
    if (viewDescriptors_[i].firstMiplevel != descriptor.firstMiplevel)
      continue;
    if (viewDescriptors_[i].miplevelCount != descriptor.miplevelCount)
      continue;
    if (viewDescriptors_[i].firstArraySlice != descriptor.firstArraySlice)
      continue;
    if (viewDescriptors_[i].arraySize != descriptor.arraySize)
      continue;
#ifdef DXMT_IOS
    if (!(viewDescriptors_[i].swizzle == descriptor.swizzle)) // spec 05 H7
      continue;
#endif
    return i;
  }
  viewDescriptors_.push_back(descriptor);
  version_ = version_ + 1;
  return i;
}

#ifdef DXMT_IOS
float
Texture::lostLevelMinLod(TextureViewKey view) {
  uint32_t lost = lostLevels();
  if (!lost)
    return 0.0f;
  std::shared_lock<dxmt::shared_mutex> lock(mutex_);
  auto &descriptor = viewDescriptors_[view];
  return mipclamp::MinLodForLostLevels(lost, descriptor.firstMiplevel, descriptor.miplevelCount);
}
#endif

Texture::Texture(const WMTTextureInfo &descriptor, WMT::Device device) :
    info_(descriptor),
    device_(device) {

  uint32_t arraySize = info_.array_length;
  switch (info_.type) {
  case WMTTextureTypeCubeArray:
  case WMTTextureTypeCube:
    arraySize = arraySize * 6;
    break;
  default:
    break;
  }

  viewDescriptors_.push_back({
      .format = info_.pixel_format,
      .type = info_.type,
      .firstMiplevel = 0,
      .miplevelCount = info_.mipmap_level_count,
      .firstArraySlice = 0,
      .arraySize = arraySize,
  });
  version_ = 1;
}

Texture::Texture(
    unsigned bytes_per_image, unsigned bytes_per_row, const WMTTextureInfo &descriptor, WMT::Device device
) :
    info_(descriptor),
    bytes_per_image_(bytes_per_image),
    bytes_per_row_(bytes_per_row),
    device_(device) {

  assert(info_.type == WMTTextureType2D);
  assert(info_.mipmap_level_count == 1);
  assert(info_.array_length == 1);

  viewDescriptors_.push_back({
      .format = info_.pixel_format,
      .type = info_.type,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });
  version_ = 1;
}

Rc<TextureAllocation>
Texture::allocate(Flags<TextureAllocationFlag> flags) {
  WMTResourceOptions options = WMTResourceStorageModeShared;
  WMTTextureInfo info = info_; // copy
  info.mach_port = 0;
  if (flags.test(TextureAllocationFlag::GpuReadonly)) {
    options |= WMTResourceHazardTrackingModeUntracked;
  }
  if (flags.test(TextureAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(TextureAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(TextureAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  info.options = options;
  if (bytes_per_image_) {
    WMTBufferInfo buffer_info;
    buffer_info.length = bytes_per_image_;
    buffer_info.options = options;
    buffer_info.memory.set(nullptr);
#if defined(__i386__) || defined(DXMT_IOS_WOW_NATIVE)
    // spec 06 B: page-aligned for newBufferWithBytesNoCopy: (16 KB pages on iOS)
    // MYIOSDECK spec 33 B2: in the ios_wow_native build too, from Part C (a d3d9
    // buffer-backed texture's LockRect hands this memory to the program).
    buffer_info.length = PlacedMemoryLength(bytes_per_image_);
    // MYIOSDECK spec 06 round 2: counted; a failure leaves no memory (mappedMemory null, the unix
    // side refuses the buffer) and the caller sees ThreadFailures move
    buffer_info.memory.set(
        guest_memory::Alloc(buffer_info.length, kPlacedMemoryAlignment, guest_memory::Use::Maps, "a linear texture")
    );
#endif
    auto buffer = device_.newBuffer(buffer_info);
    // MYIOSDECK spec 05 item 90: a texture made on its own buffer counts as the buffer's pages
    const bool made = bool(buffer);
    auto allocation =
        new TextureAllocation(this, std::move(buffer), buffer_info.memory.get(), info, bytes_per_row_, flags);
#if defined(__i386__) || defined(DXMT_IOS_WOW_NATIVE)
    allocation->placedLength = buffer_info.length;
#endif
    if (made) {
      allocation->allocatedBytes = memledger::BufferBytes(buffer_info.length);
      allocation->mem_record = memledger::Add(
          memledger::TextureRecord(info, allocation->allocatedBytes, false), static_cast<Allocation *>(allocation),
          this
      );
    }
    return allocation;
  }
  auto texture = flags.test(TextureAllocationFlag::Shared) ? device_.newSharedTexture(info) : device_.newTexture(info);
  const bool made = bool(texture);
  auto allocation = new TextureAllocation(this, std::move(texture), info, flags);
  if (made) { // MYIOSDECK spec 05 item 90: Metal's allocatedSize, which the unix side wrote back
    allocation->allocatedBytes = memledger::TextureBytes(info);
    allocation->mem_record = memledger::Add(
        memledger::TextureRecord(info, allocation->allocatedBytes, IsEmulatedBCFormat(device_, info_.pixel_format)),
        static_cast<Allocation *>(allocation), this
    );
  }
  return allocation;
}

/* MYIOSDECK spec 05 item 90 */
void
Texture::releasedByProgram() {
  memledger::MarkReleased(this);
}

Rc<TextureAllocation>
Texture::import(mach_port_t mach_port) {
  Flags<TextureAllocationFlag> flags;
  WMTTextureInfo info;
  info.mach_port = mach_port;
  auto texture = device_.newSharedTexture(info);
  // now allocation's info is populated
  // and we may check if it is consitent with texture's info (it should be)
  if (texture) {
    // doing some unnecessary checks for the sake of completeness
    if (info.options & WMTResourceStorageModeManaged) // should be always false
      flags.set(TextureAllocationFlag::GpuManaged);
    if (info.options & WMTResourceStorageModePrivate) // should be always true
      flags.set(TextureAllocationFlag::GpuPrivate);
    if (info.options & WMTResourceHazardTrackingModeUntracked)
      flags.set(TextureAllocationFlag::NoTracking);
    flags.set(TextureAllocationFlag::Shared);
    auto allocation = new TextureAllocation(this, std::move(texture), info, flags);
    allocation->mem_record = memledger::Add( // MYIOSDECK spec 05 item 90
        memledger::TextureRecord(info, memledger::TextureBytes(info), false), static_cast<Allocation *>(allocation),
        this
    );
    return allocation;
  }
  assert(texture && "failed to import shared texture");
  return nullptr;
}

TextureView &
Texture::view(TextureViewKey key) {
  return view(key, current_.ptr());
}

TextureView &
Texture::view(TextureViewKey key, TextureAllocation* allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return *allocation->cached_view_[key];
}

TextureViewKey Texture::checkViewUseArray(TextureViewKey key, bool isArray) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key];
  shared_lock = {};
  static constexpr uint32_t ARRAY_TYPE_MASK = 0b0101001010;
  if (unlikely(bool((1 << uint32_t(view.type)) & ARRAY_TYPE_MASK) != isArray)) {
    // TODO: this process can be cached
    auto new_view_desc = view;
    switch (view.type) {
    case WMTTextureType1D:
      new_view_desc.type = WMTTextureType1DArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType1DArray:
      new_view_desc.type = WMTTextureType1D;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2D:
      new_view_desc.type = WMTTextureType2DArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DArray:
      new_view_desc.type = WMTTextureType2D;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DMultisample:
      new_view_desc.type = WMTTextureType2DMultisampleArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DMultisampleArray:
      new_view_desc.type = WMTTextureType2DMultisample;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureTypeCube:
      new_view_desc.type = WMTTextureTypeCubeArray;
      new_view_desc.arraySize = 6;
      break;
    case WMTTextureTypeCubeArray:
      new_view_desc.type = WMTTextureTypeCube;
      new_view_desc.arraySize = 6;
      break;
    default:
      return key; // should be unreachable
    }
    return createView(new_view_desc);
  }
  return key;
}

#ifdef DXMT_IOS
TextureViewKey
Texture::checkViewUseSwizzle(TextureViewKey key, WMTTextureSwizzleChannels swizzle) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key];
  shared_lock = {};
  if (view.swizzle == swizzle)
    return key;
  auto new_view_desc = view;
  new_view_desc.swizzle = swizzle;
  return createView(new_view_desc);
}

TextureViewKey
Texture::checkViewUseMipRange(TextureViewKey key, uint32_t firstMiplevel, uint32_t miplevelCount) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key];
  shared_lock = {};
  if (view.firstMiplevel == firstMiplevel && view.miplevelCount == miplevelCount)
    return key;
  auto new_view_desc = view;
  new_view_desc.firstMiplevel = firstMiplevel;
  new_view_desc.miplevelCount = miplevelCount;
  return createView(new_view_desc);
}
#endif

TextureViewKey Texture::checkViewUseFormat(TextureViewKey key, WMTPixelFormat format) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key];
  shared_lock = {};
  if (unlikely(view.format != format)) {
    auto new_view_desc = view;
    new_view_desc.format = format;
    return createView(new_view_desc);
  }
  return key;
}

Rc<TextureAllocation>
Texture::rename(Rc<TextureAllocation> &&newAllocation) {
  Rc<TextureAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void Texture::incRef(){
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void Texture::decRef(){
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

RenamableTexturePool::RenamableTexturePool(
    Texture *texture, size_t capacity, Flags<TextureAllocationFlag> allocation_flags
) :
    texture_(texture),
    allocations_(capacity, nullptr),
    allocation_flags_(allocation_flags),
    capacity_(capacity) {}

void
RenamableTexturePool::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
RenamableTexturePool::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

namespace {
/* MYIOSDECK spec 05 item 90: every pool's copies beyond their first, in bytes */
std::atomic<uint64_t> g_rename_extra_bytes = {0};

#ifdef DXMT_IOS
bool
RenameCapOn() {
  static const bool on = env::readSwitch("DXMT_DEPTH_RENAME_CAP", true);
  return on;
}
#endif
} // namespace

RenamableTexturePool::~RenamableTexturePool() {
  if (extra_bytes_)
    g_rename_extra_bytes.fetch_sub(extra_bytes_, std::memory_order_relaxed);
}

uint64_t
RenamableTexturePool::extraBytesInUse() {
  return g_rename_extra_bytes.load(std::memory_order_relaxed);
}

/* MYIOSDECK spec 05 item 90: a copy's size is the first one's (Metal's allocatedSize); without it,
   8 bytes a texel, the most a depth-stencil format takes */
bool
RenamableTexturePool::reserveCopy() {
  uint64_t bytes = allocations_[0].ptr() ? allocations_[0]->allocatedBytes : 0;
  if (!bytes)
    bytes = uint64_t(texture_->width()) * texture_->height() * std::max(texture_->depth(), 1u) *
            std::max(texture_->arrayLength(), 1u) * std::max(texture_->sampleCount(), 1u) * 8;
  uint64_t in_use = g_rename_extra_bytes.load(std::memory_order_relaxed);
  do {
    if (!RenamePoolMayGrow(made_, bytes, in_use)) {
      static std::atomic<bool> logged = false;
      if (!logged.exchange(true))
        Logger::info(str::format(
            "[dxmt-mem] depth rename copies capped: ", texture_->width(), "x", texture_->height(), ", ",
            (bytes + (512u << 10)) >> 20, " MB a copy, ", made_, " kept (", kRenamePoolBytes >> 20,
            " MB a texture, ", kRenameExtraBytes >> 20, " MB of extra copies in all; DXMT_DEPTH_RENAME_CAP=0 for 32)"
        ));
      return false;
    }
  } while (!g_rename_extra_bytes.compare_exchange_weak(in_use, in_use + bytes, std::memory_order_relaxed));
  extra_bytes_ += bytes;
  return true;
}

Rc<TextureAllocation>
RenamableTexturePool::getNext(uint64_t frame) {
  if (frame > last_frame_) {
    last_frame_ = frame;
    current_index_ = 0;
  }
  auto current_index = current_index_++ % capacity_;
  if (!allocations_[current_index].ptr()) {
#ifdef DXMT_IOS
    /* MYIOSDECK spec 05 item 90: slots fill in order, so an empty one is the next copy; past the
       budgets a copy the pool has is taken instead, in turn */
    if (made_ && RenameCapOn() && !reserveCopy())
      return allocations_[current_index % made_];
#endif
    allocations_[current_index] = texture_->allocate(allocation_flags_);
    made_++;
  }
  return allocations_[current_index];
}

} // namespace dxmt