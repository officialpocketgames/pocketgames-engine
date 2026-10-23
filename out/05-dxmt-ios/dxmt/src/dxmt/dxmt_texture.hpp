#pragma once
// Changed for MYIOSDECK (spec 05 D5, H7; spec 07 A2: LGPL-2.1-or-later blocks from dxmt-d3d9
// v0.4-d3d9 (e8dd4c6), marked inline, see LICENSE-NOTES.md). Upstream: DXMT a3a6e8f, MIT.
#include "Metal.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_residency.hpp"
#include "dxmt_allocation.hpp"
#include "rc/util_rc_ptr.hpp"
#include "thread.hpp"
#include "util_flags.hpp"

namespace dxmt {

enum class TextureAllocationFlag : uint32_t {
  GpuReadonly = 0,
  NoTracking = 0,
  GpuPrivate = 1,
  CpuInvisible = 1,
  CpuWriteCombined = 2,
  OwnedByCommandList = 3,
  GpuManaged = 4,
  Shared = 5,
};

typedef unsigned TextureViewKey;

struct TextureViewDescriptor {
  WMTPixelFormat format    : 24;
  WMTTextureType type      : 8;
  uint32_t firstMiplevel   : 4 = 0;
  uint32_t miplevelCount   : 4 = 1;
  uint32_t firstArraySlice : 12 = 0;
  uint32_t arraySize       : 12 = 1;
#ifdef DXMT_IOS
  /* Spec 05 H7: a view can carry its own component swizzle. */
  WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha
  };
#endif
};

#ifdef DXMT_IOS
inline bool
operator==(const WMTTextureSwizzleChannels &a, const WMTTextureSwizzleChannels &b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
#endif

class Texture;
class TextureAllocation;

class TextureView {
public:
  virtual ~TextureView() {};

  void incRef();
  void decRef();

  WMT::Reference<WMT::Texture> texture;
  uint64_t gpuResourceID;
  DXMT_RESOURCE_RESIDENCY_STATE residency{};
  TextureAllocation *allocation; // `TextureAllocation` holds strong reference to `TextureView`
  TextureViewKey key;

  TextureView(const TextureView &) = delete;
  TextureView(TextureView &&) = delete;
  TextureView &operator=(const TextureView &) = delete;
  TextureView &operator=(TextureView &&) = delete;
  TextureView(TextureAllocation *allocation);
  TextureView(TextureAllocation *allocation, TextureViewKey key, TextureViewDescriptor descriptor);

private:
  std::atomic<uint32_t> refcount_ = {0u};
};

class TextureViewRef : public Rc<TextureView> {
public:
  using Rc<TextureView>::Rc;

  WMT::Texture
  texture() const {
    if (!*this)
      return {};
    return ptr()->texture;
  }

  TextureViewRef &
  operator=(TextureView &ref) {
    return (*this = &ref);
  }
};

class TextureAllocation : public Allocation {
  friend class Texture;

  /**
   * notes on thread-safefy:
   * all states in `TextureAllocation` is either immutable or only accessed by `dxmt-encode-thread`
   */

public:

  WMT::Texture texture() const {
    return obj_;
  }

  /* spec 07: LGPL-2.1-or-later, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6). The
     underlying linear MTLBuffer of a buffer-backed allocation (null for other
     allocations); the D3D9 frontend keeps its own reference for LockRect. */
  WMT::Buffer buffer() const {
    return buffer_;
  }

  Flags<TextureAllocationFlag>
  flags() const {
    return flags_;
  }

  Texture *descriptor;
  void *mappedMemory;
  uint64_t gpuResourceID;
  mach_port_t machPort;
  EncoderDepKey depkey;

private:
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Buffer> &&buffer, void *mapped_buffer, const WMTTextureInfo &info,
      unsigned bytes_per_row, Flags<TextureAllocationFlag> flags
  );
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Texture> &&texture, const WMTTextureInfo &textureDescriptor,
      Flags<TextureAllocationFlag> flags
  );
  ~TextureAllocation();

  TextureAllocation(const TextureAllocation &) = delete;
  TextureAllocation(TextureAllocation &&) = delete;

  WMT::Reference<WMT::Texture> obj_;
  WMT::Reference<WMT::Buffer> buffer_;
  uint32_t version_ = 0;
  Flags<TextureAllocationFlag> flags_;
  std::vector<TextureViewRef> cached_view_;
};

class Texture {

public:
  void incRef();
  void decRef();

  TextureViewKey createView(TextureViewDescriptor const &descriptor);

  constexpr TextureAllocation *
  current() {
    return current_.ptr();
  }

  WMTTextureType
  textureType() const {
    return info_.type;
  }

  WMTPixelFormat
  pixelFormat() const {
    return info_.pixel_format;
  }

  WMTTextureType
  textureType(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view].type;
  }

  WMTPixelFormat
  pixelFormat(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view].format;
  }

  WMTTextureUsage
  usage() const {
    return info_.usage;
  }

  unsigned
  sampleCount() const {
    return info_.sample_count;
  }

  unsigned
  width() const {
    return info_.width;
  }

  unsigned
  height() const {
    return info_.height;
  }

  unsigned
  depth() const {
    return info_.depth;
  }

  unsigned
  width(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return std::max(info_.width >> viewDescriptors_[view].firstMiplevel, 1u);
  }

  unsigned
  height(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return std::max(info_.height >> viewDescriptors_[view].firstMiplevel, 1u);
  }

  /**
  \warning for cube texture, arrayLength() returns 1, while arrayLength(0) returns 6"
  */
  unsigned
  arrayLength() const {
    return info_.array_length;
  }

  unsigned
  arrayLength(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view].arraySize;
  }

  /* ---- spec 07: LGPL-2.1-or-later block, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6)
     dxmt_texture.hpp ---- */
  unsigned
  depth(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return std::max(info_.depth >> viewDescriptors_[view].firstMiplevel, 1u);
  }

  unsigned
  miplevelCount() const {
    return info_.mipmap_level_count;
  }

  /* The view of the whole texture: in this tree always the first one. */
  static constexpr TextureViewKey fullView = 0;
  /* ---- end of LGPL block ---- */

  Rc<TextureAllocation> allocate(Flags<TextureAllocationFlag> flags);
  Rc<TextureAllocation> import(mach_port_t mach_port);

  TextureView &view(TextureViewKey key);
  TextureView &view(TextureViewKey key, TextureAllocation *allocation);

  TextureViewKey checkViewUseArray(TextureViewKey key, bool isArray);
  TextureViewKey checkViewUseFormat(TextureViewKey key, WMTPixelFormat format);

#ifdef DXMT_IOS
  /* Spec 05 H7: the same view key if the view already has this swizzle / mip
     range, otherwise a new (cached) view derived from it. Spec 07 uses these;
     d3d11 doesn't call them. */
  TextureViewKey checkViewUseSwizzle(TextureViewKey key, WMTTextureSwizzleChannels swizzle);
  TextureViewKey checkViewUseMipRange(TextureViewKey key, uint32_t firstMiplevel, uint32_t miplevelCount);
#endif

  Rc<TextureAllocation> rename(Rc<TextureAllocation> &&newAllocation);

  Texture(const WMTTextureInfo &info, WMT::Device device);

  Texture(unsigned bytes_per_image, unsigned bytes_per_row, const WMTTextureInfo &info, WMT::Device device);

#ifdef DXMT_IOS
  /* Spec 05 D5: number of top mip levels left out of the Metal texture.
     `info_` describes the physical texture; physical level p is logical level
     p + mipBias(). Set once, before the texture is used. */
  uint32_t
  mipBias() const {
    return mip_bias_;
  }

  void
  setMipBias(uint32_t bias) {
    mip_bias_ = bias;
  }

  /* Physical levels a copy couldn't fill (bit p = level p). */
  uint32_t
  lostLevels() const {
    return lost_levels_.load(std::memory_order_acquire);
  }

  bool
  isLevelLost(uint32_t level) const {
    return level < 32 && (lostLevels() & (1u << level));
  }

  void
  markLevelLost(uint32_t level) {
    if (level < 32)
      lost_levels_.fetch_or(1u << level, std::memory_order_acq_rel);
  }

  void
  clearLevelLost(uint32_t level) {
    if (level < 32)
      lost_levels_.fetch_and(~(1u << level), std::memory_order_acq_rel);
  }

  unsigned
  mipLevelCount() const {
    return info_.mipmap_level_count;
  }

  /* Minimum LOD for sampling through `view` so that it never starts at a lost level. */
  float lostLevelMinLod(TextureViewKey view);
#endif

private:
#ifdef DXMT_IOS
  uint32_t mip_bias_ = 0;
  std::atomic<uint32_t> lost_levels_ = 0;
#endif
  void prepareAllocationViews(TextureAllocation* allocation);

  WMTTextureInfo info_;
  unsigned bytes_per_image_ = 0;
  unsigned bytes_per_row_ = 0;

  Rc<TextureAllocation> current_;
  uint32_t version_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};

  std::vector<TextureViewDescriptor> viewDescriptors_;
  dxmt::shared_mutex mutex_;
  WMT::Device device_;
};

class RenamableTexturePool {
public:
  void incRef();
  void decRef();
  Rc<TextureAllocation> getNext(uint64_t frame_);

  RenamableTexturePool(Texture *texture, size_t capacity,Flags<TextureAllocationFlag> allocation_flags);

private:
  Rc<Texture> texture_;
  std::vector<Rc<TextureAllocation>> allocations_;
  uint64_t last_frame_ = 0;
  Flags<TextureAllocationFlag> allocation_flags_;
  unsigned capacity_;
  unsigned current_index_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

} // namespace dxmt